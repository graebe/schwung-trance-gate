/*
 * trance_gate_core.h -- the engine, with no host in it.
 *
 * WHY THIS FILE EXISTS. The Trance Gate runs in two shells now: Schwung's
 * chain on Move, and a VST3/AU plugin in a DAW. A second implementation of
 * the envelope would drift from the first within a month, and the bug reads
 * as "it sounds different in Live" -- which is the hardest kind to chase. So
 * there is one engine and two shells, and this is the engine.
 *
 * WHAT MOVED OUT OF THE SHELL. Three things were host-shaped and are now
 * parameters of the core instead:
 *
 *   SAMPLE RATE was a #define of 44100. A DAW runs at 48k or 96k, where that
 *   constant makes a 1/16 step 8.8% or 118% too long.
 *
 *   AUDIO FORMAT was int16 interleaved, which is what the Move mailbox
 *   carries. Plugins are float. Both paths are here and must agree.
 *
 *   TRANSPORT was two host callbacks. It is a struct the shell fills, so the
 *   engine cannot reach for a global that a plugin has no way to provide.
 *
 * THE PARAMETER API IS DELIBERATELY STRINGS. It is what Schwung's chain
 * already speaks, and it is what makes a patch portable: `state` emits a JSON
 * blob that the other shell parses back byte for byte. A typed C struct would
 * have been tidier and would have needed a second serialiser to go with it.
 *
 * MIT, like the module. The plugin shell around it may be GPL; that does not
 * flow back into this file.
 */
#ifndef TRANCE_GATE_CORE_H
#define TRANCE_GATE_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TG_MAX_STEPS  32
#define TG_SLOTS      8

typedef struct tg_instance tg_core_t;

/* What the shell knows about the host's clock, per block.
 *
 * `beats` is the song position in beats and `running` says whether it means
 * anything -- a stopped transport is not beat 0, it is no beat at all, and
 * conflating the two makes the gate re-trigger on every stop. */
typedef struct {
    int    running;
    double beats;
    float  bpm;
} tg_transport_t;

tg_core_t *tg_core_create(double sample_rate);
void       tg_core_destroy(tg_core_t *c);

/* Safe to call from the audio thread: it only recomputes derived lengths. */
void       tg_core_set_sample_rate(tg_core_t *c, double sample_rate);
double     tg_core_get_sample_rate(const tg_core_t *c);

/* Interleaved stereo, in place. The int16 path is Move's; the float path is
 * every plugin format's. They run the same maths and must agree. */
void tg_core_process_i16(tg_core_t *c, int16_t *lr, int frames, const tg_transport_t *t);
void tg_core_process_f32(tg_core_t *c, float   *lr, int frames, const tg_transport_t *t);

/* Non-interleaved, which is what VST3 and AU actually hand you. */
void tg_core_process_f32_split(tg_core_t *c, float *l, float *r, int frames,
                               const tg_transport_t *t);

void tg_core_set_param(tg_core_t *c, const char *key, const char *val);
/* Returns the length written, or -1 for a key this engine does not serve --
 * which is how a shell knows to answer its own (chain_params, ui_hierarchy). */
int  tg_core_get_param(tg_core_t *c, const char *key, char *buf, int buf_len);

void tg_core_on_midi(tg_core_t *c, const uint8_t *msg, int len);

#ifdef __cplusplus
}
#endif
#endif /* TRANCE_GATE_CORE_H */
