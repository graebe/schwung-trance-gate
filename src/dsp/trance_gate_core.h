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

/* 128 steps is eight bars at 1/16. The masks below are the reason this is a
 * number and not "as many as you like": a pattern is two bitmaps and a depth
 * per step, and all of it has to fit a state blob. */
#define TG_MAX_STEPS  128
#define TG_SLOTS      8

/*
 * THE RATE LADDER AND THE STAGE SCALE, for embedders that have to declare
 * host parameters before any audio runs.
 *
 * These were private to the engine while its only shell was the Move module,
 * which reads its options out of chain_params. A plugin cannot: a host wants
 * the option count and the default at construction time, long before it will
 * accept an answer from get_param.
 *
 * THEY ARE ASSERTED AGAINST THE ENGINE, not just written down -- see
 * tests/test_core.c, which walks every index, checks the labels are distinct,
 * and checks that one past the end falls back to the default. A number here
 * that drifts from the Rust table fails there rather than in a host.
 */
#define TG_NUM_RATES     13
#define TG_RATE_DEFAULT  7      /* 1/16 */
/* A stage runs from 0 to twice the gate's WIDTH, as a percentage of it. */
#define TG_STAGE_MAX_PCT 200.0f
#define TG_MASK_WORDS ((TG_MAX_STEPS + 31) / 32)

/*
 * A step bitmap. This was a bare uint32_t while 32 steps was the ceiling, and
 * widening it is the invasive half of going to 128: the compiler cannot find
 * `(p->steps >> i) & 1` for you once the type still has a `>>`. Wrapping it
 * in a struct is deliberate -- it makes every direct shift a compile error,
 * so the audit is done by the build rather than by grep.
 */
typedef struct { uint32_t w[TG_MASK_WORDS]; } tg_mask_t;

static inline int  tg_mask_get(const tg_mask_t *m, int i) {
    return (i >= 0 && i < TG_MAX_STEPS) ? (int)((m->w[i >> 5] >> (i & 31)) & 1u) : 0;
}
static inline void tg_mask_set(tg_mask_t *m, int i, int on) {
    if (i < 0 || i >= TG_MAX_STEPS) return;
    if (on) m->w[i >> 5] |=  (1u << (i & 31));
    else    m->w[i >> 5] &= ~(1u << (i & 31));
}
static inline void tg_mask_zero(tg_mask_t *m) {
    for (int k = 0; k < TG_MASK_WORDS; k++) m->w[k] = 0;
}

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

/*
 * THE AUTOMATABLE PARAMETERS, BY NUMBER.
 *
 * tg_core_set_param is the canonical door and takes strings, which is right
 * for a patch, a pattern or a pad edit -- all of them message-thread work. It
 * is wrong for HOST AUTOMATION, which arrives on the audio thread: a float
 * formatted with snprintf and parsed back with atof costs a locale-dependent
 * conversion in each direction (atof honours LC_NUMERIC, so a comma-decimal
 * host turns "0.750" into 0) and a strcmp ladder, per value, per block.
 *
 * These are the same twelve values on the same wire conventions -- slot,
 * length and rate are INDICES, legato and time_mode are 0|1, the rest are the
 * units the string keys use -- with the decimal detour removed. The string
 * setter is implemented in terms of this one, so every clamp exists once.
 *
 * THE VALUES ARE THE ABI. A host that saved an automation lane saved these
 * numbers, so inserting one in the middle silently rewires a user's project.
 */
typedef enum {
    TG_P_SLOT = 0, TG_P_LENGTH, TG_P_RATE, TG_P_LEGATO, TG_P_TIME_MODE,
    TG_P_CURVE, TG_P_AMOUNT, TG_P_HOLD, TG_P_ATTACK, TG_P_DECAY,
    TG_P_SUSTAIN, TG_P_RELEASE, TG_P_COUNT
} tg_param_t;

/* Audio-thread safe: a switch, a clamp and a store. No allocation, no
 * formatting, no locale. A param outside the enum is dropped rather than
 * clamped onto a neighbour. */
void tg_core_set_num(tg_core_t *instance, tg_param_t param, double value);

void tg_core_set_param(tg_core_t *c, const char *key, const char *val);
/* Returns the length written, or -1 for a key this engine does not serve --
 * which is how a shell knows to answer its own (chain_params, ui_hierarchy). */
int  tg_core_get_param(tg_core_t *c, const char *key, char *buf, int buf_len);

/*
 * SIZE YOUR get_param BUFFER FROM THIS, DO NOT PICK A NUMBER.
 *
 * The longest thing the engine emits is the "state" blob, and it grew with
 * TG_MAX_STEPS: eight slots of fully accented 128-step patterns is ~2.6 KB,
 * where 32-step ones were a few hundred bytes. A shell that had guessed 2048
 * would not fail -- get_param snprintfs, so it TRUNCATES, and a truncated
 * patch is a project that silently reloads with the wrong pattern.
 *
 * The private encoding's true worst case is asserted against this number by
 * tests/test_core.c, which builds the largest patch the format can express
 * (eight slots of fully accented 128-step patterns) and measures what the
 * emitter actually writes. That was a _Static_assert over the format macros
 * while the engine was C; measuring the emitter is the stronger of the two,
 * and it is what stands between growing the format and a corrupt save
 * downstream.
 */
#define TG_STATE_MAX 4096

/* The envelope's curve shapes, and the warp each one applies to a stage's
 * 0..1 progress. Exposed for the tests: the properties every stage depends on
 * are cheaper to assert here than to infer from rendered audio. */
enum { TG_CURVE_LINEAR = 0, TG_CURVE_EXP = 1, TG_CURVE_SCURVE = 2 };
/*
 * The playhead's position in the pattern, 0..1. Cheap and allocation-free --
 * for a caller on the audio thread, where the equivalent get_param key's
 * snprintf does not belong.
 */
double tg_core_phase01(const tg_core_t *c);

double tg_test_shape(int curve, double t);
double tg_test_shape_inv(int curve, double w);

void tg_core_on_midi(tg_core_t *c, const uint8_t *msg, int len);

#ifdef __cplusplus
}
#endif
#endif /* TRANCE_GATE_CORE_H */
