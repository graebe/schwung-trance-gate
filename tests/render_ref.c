/*
 * Render the gate over a known signal and write raw s16le stereo to stdout.
 *
 * THE POINT IS BIT-EXACTNESS ACROSS A REFACTOR. The engine is about to be
 * split so a plugin can share it, and "it still sounds right" is not a claim
 * anyone can check by ear a month later. So: a fixed patch, a deterministic
 * input, a fixed fake transport, and a byte-comparable output. If one sample
 * moves, the split changed the sound and the diff says so immediately.
 *
 * Deliberately NOT a unit test -- it is the reference the unit tests are
 * checked against, and it is also what feeds the VST port's A/B.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "audio_fx_api_v2.h"

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host);

#define SR      44100.0
#define BLOCK   128

static double g_beats = 0.0;
static float  g_bpm   = 123.0f;          /* not 120: a tempo that does not
                                          * divide the block size evenly is
                                          * where phase bugs actually show */
static float  fake_bpm(void)   { return g_bpm; }
static double fake_beats(void) { return g_beats; }

int main(int argc, char **argv) {
    double seconds = (argc > 1) ? atof(argv[1]) : 4.0;

    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.get_bpm = fake_bpm;
    host.get_beat_position = fake_beats;

    audio_fx_api_v2_t *api = move_audio_fx_init_v2(&host);
    void *inst = api->create_instance(NULL, NULL);

    /* A patch that exercises every branch: gaps, a tie, per-step amounts,
     * a real envelope and a gate length below 1. */
    api->set_param(inst, "rate",        "1/16");
    api->set_param(inst, "length",      "15");     /* index -> 16 steps */
    api->set_param(inst, "pattern",     "BEEF");
    api->set_param(inst, "ties",        "0022");
    api->set_param(inst, "attack",      "3.5");
    api->set_param(inst, "decay",       "40");
    api->set_param(inst, "sustain",     "0.6");
    api->set_param(inst, "release",     "25");
    api->set_param(inst, "hold",        "0.75");
    api->set_param(inst, "amount",      "0.9");
    for (int s = 0; s < 16; s++) {                 /* per-step amounts vary */
        char k[8], v[16];
        snprintf(k, sizeof(k), "cursor");
        snprintf(v, sizeof(v), "%d", s);
        api->set_param(inst, k, v);
        snprintf(v, sizeof(v), "%.3f", 0.35 + 0.04 * s);
        api->set_param(inst, "step_amount", v);
    }
    api->set_param(inst, "cursor", "0");

    int total = (int)(seconds * SR);
    int16_t buf[BLOCK * 2];
    double phase = 0.0;
    const double w = 2.0 * M_PI * 220.0 / SR;      /* 220 Hz, so gating is
                                                    * plainly visible in the
                                                    * waveform */
    for (int done = 0; done < total; done += BLOCK) {
        int n = (total - done) < BLOCK ? (total - done) : BLOCK;
        for (int i = 0; i < n; i++) {
            short v = (short)lrint(22000.0 * sin(phase));
            phase += w;
            buf[i * 2] = v;
            buf[i * 2 + 1] = v;
        }
        api->process_block(inst, buf, n);
        fwrite(buf, sizeof(int16_t), (size_t)n * 2, stdout);
        g_beats += (n / SR) * (g_bpm / 60.0);
    }
    api->destroy_instance(inst);
    return 0;
}
