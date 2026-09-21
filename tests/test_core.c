/*
 * Tests for the portable engine -- the three things the split made possible
 * and the shell could never exercise.
 *
 * The Schwung shell runs at 44100 with int16 interleaved buffers and reads
 * the transport from two host callbacks. None of that is true in a DAW, so
 * none of it was ever tested. These are the freedoms the core now has, and
 * each one is a way for the plugin to sound different from the hardware.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "trance_gate_core.h"

static int failures = 0;
static void check(const char *what, int ok) {
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}
static void check_near(const char *what, double got, double want, double tol) {
    int ok = fabs(got - want) <= tol;
    printf("  %-58s %s (got %.3f want %.3f)\n", what, ok ? "ok" : "FAIL", got, want);
    if (!ok) failures++;
}

/* A gate that is fully open on step 0 and fully shut on step 1, with no
 * envelope at all -- so the first sample that drops tells us exactly where the
 * step boundary fell, in samples. */
static tg_core_t *mk(double sr) {
    tg_core_t *c = tg_core_create(sr);
    tg_core_set_param(c, "rate",    "1/16");
    tg_core_set_param(c, "length",  "1");      /* index -> 2 steps */
    tg_core_set_param(c, "pattern", "1");      /* step 0 on, step 1 off */
    tg_core_set_param(c, "ties",    "0");
    tg_core_set_param(c, "attack",  "0");
    tg_core_set_param(c, "decay",   "0");
    tg_core_set_param(c, "sustain", "1");
    tg_core_set_param(c, "release", "0");
    tg_core_set_param(c, "hold",    "1");
    tg_core_set_param(c, "amount",  "1");
    return c;
}

/* How many frames of full-scale DC pass before the gate shuts. */
static int frames_until_gate_shuts(double sr, float bpm) {
    tg_core_t *c = mk(sr);
    tg_transport_t t = { 1, 0.0, bpm };
    const int BL = 64;
    float buf[64 * 2];
    int n = 0;
    for (int blk = 0; blk < 4000; blk++) {
        for (int i = 0; i < BL * 2; i++) buf[i] = 1.0f;
        tg_core_process_f32(c, buf, BL, &t);
        for (int i = 0; i < BL; i++) {
            if (buf[i * 2] < 0.5f) { tg_core_destroy(c); return n + i; }
        }
        n += BL;
        t.beats += (BL / sr) * (bpm / 60.0);
    }
    tg_core_destroy(c);
    return -1;
}

int main(void) {
    /*
     * SAMPLE RATE. A 1/16 step at 120 BPM is 0.125 s. The constant this
     * replaced was 44100, which at 48k makes every step 8.8% short and at 96k
     * makes it less than half as long -- the gate would simply run at the
     * wrong tempo, which is the one bug nobody would file as "a bug".
     */
    printf("sample rate:\n");
    struct { double sr; int want; } cases[] = {
        { 44100.0, 5512 }, { 48000.0, 6000 }, { 96000.0, 12000 },
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        char what[80];
        snprintf(what, sizeof(what), "%.0f Hz: one 1/16 step at 120 BPM", cases[i].sr);
        int got = frames_until_gate_shuts(cases[i].sr, 120.0f);
        /* +-1 frame: the boundary can land mid-sample. */
        check_near(what, got, cases[i].want, 1.0);
    }
    {
        tg_core_t *c = mk(48000.0);
        check("the rate is readable back", tg_core_get_sample_rate(c) == 48000.0);
        tg_core_set_sample_rate(c, 96000.0);
        check("and settable at runtime (prepareToPlay)", tg_core_get_sample_rate(c) == 96000.0);
        tg_core_set_sample_rate(c, 0.0);
        check("a nonsense rate is refused, not stored", tg_core_get_sample_rate(c) == 96000.0);
        tg_core_destroy(c);
    }

    /*
     * THE TWO BUFFER FORMATS MUST AGREE. If they do not, the plugin is not
     * the same effect as the hardware -- which is the entire premise.
     */
    printf("float and int16 agree:\n");
    {
        tg_core_t *a = mk(44100.0), *b = mk(44100.0);
        tg_transport_t t = { 1, 0.0, 123.0f };
        const int BL = 128;
        int16_t bi[128 * 2];
        float   bf[128 * 2];
        double worst = 0.0;
        for (int blk = 0; blk < 400; blk++) {
            for (int i = 0; i < BL; i++) {
                double v = 0.8 * sin(2.0 * M_PI * 220.0 * (blk * BL + i) / 44100.0);
                bi[i * 2] = bi[i * 2 + 1] = (int16_t)lrint(v * 32767.0);
                bf[i * 2] = bf[i * 2 + 1] = (float)v;
            }
            tg_core_process_i16(a, bi, BL, &t);
            tg_core_process_f32(b, bf, BL, &t);
            for (int i = 0; i < BL; i++) {
                double d = fabs(bi[i * 2] / 32767.0 - bf[i * 2]);
                if (d > worst) worst = d;
            }
            t.beats += (BL / 44100.0) * (123.0 / 60.0);
        }
        /* One int16 quantisation step is 1/32767 ~ 3.05e-5; allow two. */
        check_near("worst sample divergence is within quantisation", worst * 32767.0, 0.0, 2.0);
        tg_core_destroy(a); tg_core_destroy(b);
    }

    /* Interleaved and split must be the same maths, or VST3 and Move differ. */
    printf("interleaved and split agree:\n");
    {
        tg_core_t *a = mk(48000.0), *b = mk(48000.0);
        tg_transport_t t = { 1, 0.0, 120.0f };
        const int BL = 100;
        float inter[100 * 2], L[100], R[100];
        double worst = 0.0;
        for (int blk = 0; blk < 200; blk++) {
            for (int i = 0; i < BL; i++) {
                float v = (float)sin(0.01 * (blk * BL + i));
                inter[i * 2] = inter[i * 2 + 1] = v;
                L[i] = R[i] = v;
            }
            tg_core_process_f32(a, inter, BL, &t);
            tg_core_process_f32_split(b, L, R, BL, &t);
            for (int i = 0; i < BL; i++) {
                double d = fabs(inter[i * 2] - L[i]);
                if (d > worst) worst = d;
                d = fabs(inter[i * 2 + 1] - R[i]);
                if (d > worst) worst = d;
            }
            t.beats += (BL / 48000.0) * (120.0 / 60.0);
        }
        check("bit-identical across the two float layouts", worst == 0.0);
        tg_core_destroy(a); tg_core_destroy(b);
    }

    /*
     * A STOPPED TRANSPORT IS NOT BEAT 0. The shell used to detect this with a
     * negative beat position; the core is handed a flag. Getting it wrong
     * makes the gate re-trigger on every stop, and passes audio when it
     * should hold open.
     */
    printf("transport:\n");
    {
        tg_core_t *c = mk(44100.0);
        tg_transport_t stopped = { 0, 0.0, 120.0f };
        float buf[64 * 2];
        for (int i = 0; i < 64 * 2; i++) buf[i] = 1.0f;
        tg_core_process_f32(c, buf, 64, &stopped);
        check("stopped holds the gate open (passes dry)", buf[0] == 1.0f && buf[127] == 1.0f);
        tg_core_destroy(c);
    }

    printf(failures ? "\nFAILED (%d)\n" : "\nPASS\n", failures);
    return failures ? 1 : 0;
}
