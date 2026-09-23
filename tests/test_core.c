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

/* One colon-separated field out of the `ui` readout, as a number. The layout
 * is steps:ties:length:phase:ms_step:advancing:cursor:depths. */
static double ui_field(tg_core_t *c, int index) {
    char buf[1024];
    if (tg_core_get_param(c, "ui", buf, sizeof(buf)) < 0) return -1.0;
    const char *p = buf;
    for (int i = 0; i < index; i++) {
        p = strchr(p, ':');
        if (!p) return -1.0;
        p++;
    }
    return atof(p);
}

/*
 * `frames` frames of DC through the SPLIT path, transport running, returned
 * as one float per frame. Callers free it.
 *
 * Split and not interleaved on purpose: with the interleaved path an index
 * into the buffer is half a frame, and a test that indexes it as if it were
 * mono reads the first half of its render twice over -- which is subtle,
 * plausible-looking, and quietly passed against the level-latching bug.
 */
static float *render_dc(tg_core_t *c, int frames, float bpm) {
    float *l = (float *)malloc(sizeof(float) * (size_t)frames);
    float *r = (float *)malloc(sizeof(float) * (size_t)frames);
    tg_transport_t t;
    t.running = 1; t.bpm = bpm;
    for (int off = 0; off < frames; off += 64) {
        int n = (frames - off) < 64 ? (frames - off) : 64;
        for (int i = 0; i < n; i++) { l[off + i] = 1.0f; r[off + i] = 1.0f; }
        t.beats = (double)off / 44100.0 * ((double)bpm / 60.0);
        tg_core_process_f32_split(c, l + off, r + off, n, &t);
    }
    free(r);
    return l;
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

    /*
     * 128 STEPS. The masks were one uint32 and are now four, and the way that
     * breaks is silently: a step past 31 lands in the wrong word, or the top
     * word is never read, and the pattern simply misses beats nobody counts.
     */
    printf("128 steps:\n");
    {
        tg_core_t *c = tg_core_create(44100.0);
        tg_core_set_param(c, "length", "127");        /* index -> 128 steps */
        char buf[64];
        tg_core_get_param(c, "length", buf, sizeof(buf));
        check("length reaches 128", atoi(buf) == 127);

        /* Set every step individually and read it back -- the only test that
         * catches a word-index bug at a boundary (31/32, 63/64, 95/96). */
        int bad = -1;
        for (int i = 0; i < 128; i++) {
            snprintf(buf, sizeof(buf), "%d", i);
            tg_core_set_param(c, "cursor", buf);
            tg_core_set_param(c, "step", (i % 3 == 0) ? "On" : "Off");
        }
        for (int i = 0; i < 128 && bad < 0; i++) {
            snprintf(buf, sizeof(buf), "%d", i);
            tg_core_set_param(c, "cursor", buf);
            tg_core_get_param(c, "step", buf, sizeof(buf));
            const char *want = (i % 3 == 0) ? "On" : "Off";
            if (strcmp(buf, want) != 0) bad = i;
        }
        check("every one of the 128 steps round-trips", bad < 0);
        if (bad >= 0) printf("      first wrong step: %d\n", bad);

        /* Step 127 specifically: the highest bit of the highest word. */
        tg_core_set_param(c, "cursor", "127");
        tg_core_set_param(c, "step", "Tie");
        tg_core_get_param(c, "step", buf, sizeof(buf));
        check("step 127 -- top bit of the top word -- holds a tie", strcmp(buf, "Tie") == 0);
        tg_core_destroy(c);
    }

    /* A <=32-step pattern must still emit the hex the previous version did,
     * or a patch stops moving between builds. */
    printf("hex compatibility:\n");
    {
        tg_core_t *c = tg_core_create(44100.0);
        char buf[64];
        tg_core_set_param(c, "pattern", "5555");
        tg_core_get_param(c, "pattern", buf, sizeof(buf));
        check("a 16-step mask still reads back as \"5555\"", strcmp(buf, "5555") == 0);
        tg_core_set_param(c, "pattern", "FFFFFFFFFFFFFFFF");   /* 64 bits */
        tg_core_get_param(c, "pattern", buf, sizeof(buf));
        check("a 64-bit mask survives the round trip",
              strcmp(buf, "FFFFFFFFFFFFFFFF") == 0);
        tg_core_destroy(c);
    }

    /*
     * LEGATO. Below Sustain 100% an untied ON step re-articulates; legato
     * makes it hold. At 100% there is nothing to hear, which is why this
     * measures at 40%.
     */
    printf("legato:\n");
    {
        double lvl[2];
        for (int leg = 0; leg < 2; leg++) {
            tg_core_t *c = tg_core_create(44100.0);
            tg_core_set_param(c, "rate", "1/16");
            tg_core_set_param(c, "length", "1");       /* 2 steps, both ON */
            tg_core_set_param(c, "pattern", "3");
            tg_core_set_param(c, "ties", "0");
            tg_core_set_param(c, "attack", "30");
            tg_core_set_param(c, "decay", "1");
            tg_core_set_param(c, "sustain", "0.4");
            tg_core_set_param(c, "release", "0");
            tg_core_set_param(c, "hold", "1");
            tg_core_set_param(c, "amount", "1");
            tg_core_set_param(c, "legato", leg ? "1" : "0");

            tg_transport_t t = { 1, 0.0, 120.0f };
            const int BL = 64; float b[64 * 2];
            /* settle through step 0, then measure the first ms of step 1 */
            /* A 1/16 step at 120 BPM is 5512 samples = ~86 blocks of 64, so
             * step 1 begins at block 86. Measuring earlier than that measures
             * step 0 and reports no difference whatever legato does. */
            double acc = 0; int cnt = 0;
            for (int blk = 0; blk < 170; blk++) {
                for (int i = 0; i < BL * 2; i++) b[i] = 1.0f;
                tg_core_process_f32(c, b, BL, &t);
                if (blk >= 88 && blk < 150) { for (int i = 0; i < BL; i++) { acc += b[i*2]; cnt++; } }
                t.beats += (BL / 44100.0) * 2.0;
            }
            lvl[leg] = acc / cnt;
            tg_core_destroy(c);
        }
        printf("      step 1 level: legato off %.3f, on %.3f\n", lvl[0], lvl[1]);
        /* OFF retriggers: attack climbs 0.4 -> 1.0 each step, so the mean sits
         * ABOVE sustain. ON holds flat AT sustain. The sign matters -- getting
         * it backwards is how a switch that does nothing looks like it works. */
        check("legato OFF re-articulates (mean above sustain)", lvl[0] > 0.45);
        check("legato ON holds flat at sustain", lvl[1] < 0.45);
        check("and the two genuinely differ", lvl[0] - lvl[1] > 0.05);
    }

    /*
     * STATE SIZE. The slot budget is 8192 and a bus insert's is 1024, and an
     * oversized blob is DROPPED rather than truncated -- so the number worth
     * knowing is where the bus case stops working, not whether it does.
     */
    printf("state size:\n");
    {
        tg_core_t *c = tg_core_create(44100.0);
        char buf[8192];
        int plain = 0, accented = 0;
        for (int s = 0; s < 8; s++) {
            char v[16]; snprintf(v, sizeof(v), "%d", s);
            tg_core_set_param(c, "slot", v);
            tg_core_set_param(c, "length", "127");
            tg_core_set_param(c, "pattern", "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");
        }
        plain = tg_core_get_param(c, "state", buf, sizeof(buf));
        printf("      8 slots x 128 steps, no accents : %5d bytes\n", plain);
        for (int s = 0; s < 8; s++) {
            char v[16]; snprintf(v, sizeof(v), "%d", s);
            tg_core_set_param(c, "slot", v);
            for (int i = 0; i < 128; i++) {
                snprintf(v, sizeof(v), "%d", i);
                tg_core_set_param(c, "cursor", v);
                tg_core_set_param(c, "step_amount", "0.5");
            }
        }
        accented = tg_core_get_param(c, "state", buf, sizeof(buf));
        printf("      the same, every step accented   : %5d bytes\n", accented);
        check("worst case fits an audio FX slot (8192)", accented > 0 && accented <= 8192);
        check("no-accent case fits a bus insert (1024)", plain > 0 && plain <= 1024);
        if (accented > 1024)
            printf("      note: a fully accented 8x128 patch exceeds a BUS INSERT's\n"
                   "            1024 bytes. Slots are unaffected.\n");
        tg_core_destroy(c);
    }

    /*
     * THE STEP DURATION IS KNOWN BEFORE ANY AUDIO RUNS, and follows the rate
     * immediately. The `ui` readout is where a UI learns it, and the plugin
     * draws its envelope against it -- so a value that only appeared after
     * the first block meant a blank panel on open, and one that only updated
     * on a block meant the picture disagreed with the Rate knob while the
     * transport sat still.
     */
    printf("step duration in the ui readout:\n");
    {
        tg_core_t *c = tg_core_create(44100.0);

        check_near("1/16 at 120 BPM reads 125 ms before any block",
                   ui_field(c, 4), 125.0, 0.5);

        tg_core_set_param(c, "rate", "1/32");
        check_near("...and follows the rate with no block between",
                   ui_field(c, 4), 62.5, 0.5);

        tg_core_set_param(c, "rate", "1/4");
        check_near("...at the slow end too", ui_field(c, 4), 500.0, 0.5);

        /* A block at a real tempo replaces the nominal 120. */
        float lr[128];
        for (int i = 0; i < 128; i++) lr[i] = 0.0f;
        tg_transport_t t;
        t.running = 1; t.beats = 0.0; t.bpm = 174.0f;
        tg_core_process_f32(c, lr, 64, &t);
        check_near("a block hands over the host's real tempo",
                   ui_field(c, 4), 60000.0 / 174.0, 1.0);

        tg_core_destroy(c);
    }

    /*
     * THE STRUCK STEP'S LEVEL OWNS THE WHOLE GATE.
     *
     * The level used to be read as depth[current_step] every sample, so a
     * release that outlived its step was scaled by the NEXT step's amount --
     * pulling a pad to 25% quietened the body of its envelope and left the
     * tail at whatever followed. Two steps at 100% and 25% and a release long
     * enough to cross the boundary is the smallest case that shows it.
     */
    printf("the level is latched at gate-open:\n");
    {
        tg_core_t *c = tg_core_create(44100.0);
        tg_core_set_param(c, "rate",    "1/16");
        tg_core_set_param(c, "length",  "1");      /* index -> 2 steps */
        tg_core_set_param(c, "pattern", "1");      /* step 0 on, step 1 off */
        tg_core_set_param(c, "ties",    "0");
        tg_core_set_param(c, "attack",  "0");
        tg_core_set_param(c, "decay",   "0");
        tg_core_set_param(c, "sustain", "1");
        tg_core_set_param(c, "hold",    "1");
        tg_core_set_param(c, "amount",  "1");
        /* Step 0 struck at a QUARTER, step 1 (where the tail lands) at full. */
        tg_core_set_param(c, "cursor", "0");
        tg_core_set_param(c, "step_amount", "0.25");
        tg_core_set_param(c, "cursor", "1");
        tg_core_set_param(c, "step_amount", "1.00");
        /* One step is 125 ms at 120 BPM; a 60 ms release from the step edge
         * spends half its life inside step 1. */
        tg_core_set_param(c, "release", "60");

        /* Two steps of DC through the SPLIT path, so an index is a frame and
         * not half of one -- the interleaved path with mono indexing is how
         * the first draft of this test passed against the very bug it is
         * here to catch. */
        const int spb = (int)(44100.0 * 0.125);
        float *buf = render_dc(c, spb * 2, 120.0f);

        /* The gate opened at a quarter, so nothing it produces -- body or
         * tail -- may exceed a quarter. Before the fix the tail climbed
         * towards step 1's full level instead of decaying from step 0's. */
        float peakTail = 0.0f;
        for (int i = spb; i < spb + spb / 2; i++)
            if (buf[i] > peakTail) peakTail = buf[i];
        check_near("the release tail stays at the struck step's level",
                   peakTail, 0.25, 0.02);
        tg_core_destroy(c);
        free(buf);
    }

    /*
     * A TIE IS ONE GATE, so it holds ONE level -- the struck step's. A level
     * that stepped mid-gate was a discontinuity in the gain, which is a click.
     */
    printf("a tie holds one level:\n");
    {
        tg_core_t *c = tg_core_create(44100.0);
        tg_core_set_param(c, "rate",    "1/16");
        tg_core_set_param(c, "length",  "1");
        tg_core_set_param(c, "pattern", "3");      /* both steps on */
        tg_core_set_param(c, "ties",    "1");      /* step 0 holds through */
        tg_core_set_param(c, "attack",  "0");
        tg_core_set_param(c, "decay",   "0");
        tg_core_set_param(c, "sustain", "1");
        tg_core_set_param(c, "release", "0");
        tg_core_set_param(c, "hold",    "1");
        tg_core_set_param(c, "amount",  "1");
        tg_core_set_param(c, "cursor", "0");
        tg_core_set_param(c, "step_amount", "0.50");
        tg_core_set_param(c, "cursor", "1");
        tg_core_set_param(c, "step_amount", "1.00");

        const int spb = (int)(44100.0 * 0.125);
        float *buf = render_dc(c, spb * 2, 120.0f);
        /* Sample either side of the boundary: one gate, one level. */
        check_near("before the tie's boundary", buf[spb - 100], 0.50, 0.02);
        check_near("...and after it, unchanged", buf[spb + 100], 0.50, 0.02);
        tg_core_destroy(c);
        free(buf);
    }

    /*
     * % MODE: THE SAME SHAPE AT EVERY RATE.
     *
     * The whole claim of the feature. In ms an envelope is absolute, so
     * halving the step halves how much of it fits; in % the stages are a
     * fraction OF the step, so the picture at 1/16 and at 1/64 is the same
     * picture with a different clock.
     */
    printf("%% mode follows the step:\n");
    {
        double lvl[2];
        for (int pass = 0; pass < 2; pass++) {
            tg_core_t *c = tg_core_create(44100.0);
            tg_core_set_param(c, "rate", pass == 0 ? "1/16" : "1/64");
            tg_core_set_param(c, "length",  "0");    /* one step, repeating */
            tg_core_set_param(c, "pattern", "1");
            tg_core_set_param(c, "ties",    "0");
            tg_core_set_param(c, "time_mode", "1");  /* % of the step */
            tg_core_set_param(c, "attack",  "0");
            tg_core_set_param(c, "decay",   "250");  /* 250/500 -> 50% of it */
            tg_core_set_param(c, "sustain", "0");
            tg_core_set_param(c, "release", "0");
            tg_core_set_param(c, "hold",    "1");
            tg_core_set_param(c, "amount",  "1");

            const double stepS = (pass == 0) ? 0.125 : 0.125 / 4.0;
            const int spb = (int)(44100.0 * stepS);
            float *buf = render_dc(c, spb, 120.0f);
            /* A quarter of the way in, a 50%-of-step decay is half spent. */
            lvl[pass] = buf[spb / 4];
            tg_core_destroy(c);
            free(buf);
        }
        printf("      1/16 %.3f   1/64 %.3f\n", lvl[0], lvl[1]);
        check_near("a quarter into the step reads the same at 1/16 and 1/64",
                   lvl[1], lvl[0], 0.03);
    }

    /* A patch written before % mode existed says nothing about it, and ms is
     * what those patches meant. */
    printf("an old patch still means milliseconds:\n");
    {
        tg_core_t *c = tg_core_create(44100.0);
        tg_core_set_param(c, "state",
            "{\"sv\":3,\"slot\":0,\"rate\":\"1/16\",\"attack\":2.00,"
            "\"decay\":20.00,\"sustain\":1.000,\"release\":20.00,"
            "\"hold\":1.000,\"amount\":1.000,\"legato\":0}");
        char buf[16];
        tg_core_get_param(c, "time_mode", buf, sizeof(buf));
        check("a blob with no tmode loads as ms", atoi(buf) == 0);
        tg_core_destroy(c);
    }

    printf(failures ? "\nFAILED (%d)\n" : "\nPASS\n", failures);
    return failures ? 1 : 0;
}
