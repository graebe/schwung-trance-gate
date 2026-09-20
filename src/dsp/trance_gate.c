/*
 * Trance Gate -- a tempo-locked step gate with a per-step ADSR.
 *
 * Modelled on the Kilohearts Trance Gate: 8 pattern slots, a pattern of up to
 * 32 steps with ties, a Resolution (the length of one step), an ADSR applied
 * at each step, and a Mix. Depth is the one addition -- it sets how far down
 * the gate closes, which turns the same module into a rhythmic ducker.
 *
 * ===========================================================================
 * THREADING. Every entry point in this file runs on the SPI audio callback:
 * SCHED_FIFO 70, pinned to core 3, ~2370us of slack per 128-frame block.
 * create_instance, destroy_instance, set_param, get_param, on_midi and
 * process_block -- all of them. There is no control thread.
 *
 * So: no malloc outside create_instance, no file I/O, no locks, no syscalls,
 * and NO LOGGING AT ALL -- including host->log, which buffers. See the
 * contract at the top of plugin_api_v1.h.
 * ===========================================================================
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include "audio_fx_api_v2.h"

#define SAMPLE_RATE   44100.0
#define TG_MAX_STEPS  32
#define TG_SLOTS      8

/* Bumped when the meaning of the `state` blob changes. Absent means 1.
 * The Ducker shipped without this and a 0.1.x blob's envelope times, read as
 * the units 0.2.0 introduced, collapsed the effect silently on a patch that
 * had worked. A version field costs nothing and is the only thing that can
 * tell one reading of a number from another. */
/* 2: patterns gained a PER-STEP DEPTH array. A v1 blob has none, and the
 * absent value must load as FULL -- loading it as zero would silently mute
 * every gate in every patch that already works. */
/* 3: `depth` is GONE, folded into `mix`.
 *
 * They were the same control wearing two names. Writing the signal path out:
 *
 *     out = in*(1-mix) + in*mix*gain,  gain = 1 - sd*depth*(1-env)
 *         = in * (1 - mix*sd*depth*(1-env))
 *
 * Only the PRODUCT mix*depth survives, so the pair spanned exactly one degree
 * of freedom and any two settings with the same product were indistinguishable
 * -- depth 0.5 / mix 1.0 and depth 1.0 / mix 0.5 are the same audio. Two knobs
 * for one quantity is worse than one: it invites hunting for a difference that
 * is not there.
 *
 * A v2 blob carries both, and the migration MULTIPLIES them, so a saved patch
 * sounds identical after the collapse rather than jumping to full wet. */
#define TG_STATE_VERSION 3
#define TG_DEPTH_FULL    255

/* What one per-slot field EMITS at worst:
 * "<8 hex steps>:<8 hex ties>:<2 digit length>:<2 hex per step>". */
#define TG_STATE_FIELD_EMIT (8 + 1 + 8 + 1 + 2 + 1 + 2 * TG_MAX_STEPS)
/* The buffer that READS one back, with room for the terminator and slack. */
#define TG_STATE_FIELD_MAX  (TG_STATE_FIELD_EMIT + 16)

/*
 * A BUS INSERT'S STATE IS CAPPED AT 1024 BYTES, AND AN OVERSIZED BLOB IS LEFT
 * ABSENT RATHER THAN TRUNCATED (chain_internal.h, MAX_BUS_FX_STATE_LEN) -- the
 * patch would load with default patterns and nothing would say why.
 *
 * Worst case here is ~892 of those 1024, so the headroom is about 13%: one
 * more pattern slot, or sixteen more steps, and a Trance Gate in a bus insert
 * silently stops remembering anything. The assert is what makes that a build
 * failure instead of a field report.
 */
#define TG_STATE_HEADER_MAX 140
#define TG_STATE_WORST_CASE \
    (TG_STATE_HEADER_MAX + TG_SLOTS * (sizeof(",\"p0\":\"\"") + TG_STATE_FIELD_EMIT))
_Static_assert(TG_STATE_WORST_CASE <= 1024,
               "state blob can exceed a bus insert's 1024-byte cap; "
               "shrink the encoding or TG_SLOTS");

enum { TG_IDLE = 0, TG_ATTACK, TG_DECAY, TG_SUSTAIN, TG_RELEASE };

/* ---------------------------------------------------------------- rates --
 *
 * THE LABEL IS THE WIRE VALUE, NOT THE INDEX. `rate` is declared to the host
 * as type "rate", whose option list the host GENERATES (buildRateParamMeta in
 * shadow_ui.js) from include_bars / include_triplets. Reporting an index would
 * couple this table's order to that generator's, and a drift between them is
 * not a visible error -- it is the gate running at the wrong subdivision with
 * the right word on screen. Reporting the label means the host resolves by
 * name and the two cannot disagree.
 *
 * Beats per step, with 1/4 == 1 beat. The triplet values match lfo_common.h's
 * table exactly (1/1T 2.667, 1/2T 1.333, ...) so a rate reads the same here as
 * it does on an LFO. Bars are excluded: a bar-long step is not a gate. */
typedef struct { const char *label; double beats; } tg_rate_t;

static const tg_rate_t tg_rates[] = {
    { "1/1T",  8.0 / 3.0  },
    { "1/2",   2.0        },
    { "1/2T",  4.0 / 3.0  },
    { "1/4",   1.0        },
    { "1/4T",  2.0 / 3.0  },
    { "1/8",   0.5        },
    { "1/8T",  1.0 / 3.0  },
    { "1/16",  0.25       },   /* index 7 -- the default */
    { "1/16T", 1.0 / 6.0  },
    { "1/32",  0.125      },
    { "1/32T", 1.0 / 12.0 },
    { "1/64",  0.0625     },
};
#define TG_NUM_RATES ((int)(sizeof(tg_rates) / sizeof(tg_rates[0])))
#define TG_RATE_DEFAULT 7

typedef struct {
    uint32_t steps;   /* bit i set = step i is ON                      */
    uint32_t ties;    /* bit i set = step i holds through into step i+1 */
    int      length;  /* 1..32                                          */
    /* Per-step level, 0..255. An ACCENT: the global `depth` scales the whole
     * sequence on top of it, so this says "how much of the gate" and depth
     * says "how much gating". 255 is the neutral value, which is why a v1
     * blob without the array must fill it rather than zero it. */
    uint8_t  depth[TG_MAX_STEPS];
} tg_pattern_t;

typedef struct tg_instance {
    tg_pattern_t pat[TG_SLOTS];
    int   slot;           /* 0..7, Kilohearts "Pattern Select" */
    int   rate_idx;
    float attack_ms;
    float decay_ms;
    float sustain;        /* 0..1 -- a LEVEL, not a duration */
    float release_ms;
    /*
     * How much of a step the gate stays open, 0..1.
     *
     * SUSTAIN IS A LEVEL AND HAS NO LENGTH -- in an ADSR it simply holds until
     * the note ends, and here "the note" is the step. That is correct and it
     * is also not what someone reaching for a shorter gate wants. This is the
     * control they are reaching for: release begins this far into the step
     * rather than at its end, which is a sequencer's gate length.
     *
     * 1.0 is the old behaviour exactly -- release at the boundary -- so it
     * costs a default, not a migration.
     */
    float hold;
    /* How much the gate acts, 0..1. 1 == a closed gate is silent, 0 == the
     * effect is bypassed. This is the old `depth` AND the old `mix`: they were
     * one quantity with two names (see TG_STATE_VERSION), so it has one now. */
    float amount;
    int   cursor;         /* edit position on the ring, 0..length-1 */

    /* Runtime. Not saved. */
    double step_pos;      /* absolute fractional step position */
    int    last_step;     /* step index at the previous sample; -1 = none */
    int    was_running;
    int    env_stage;
    /* Position through the current stage as 0..1, advanced by a PRECOMPUTED
     * reciprocal. It used to hold a sample COUNT and divide by the stage
     * length every sample -- a double division per sample, per channel pair,
     * for a quotient whose denominator cannot change inside a stage. */
    double env_t;
    double env_inc;       /* 1 / stage length in samples; 0 for a zero stage */
    float  env;           /* 0..1 */
    float  rel_from;      /* env level when RELEASE began */
    /* ATTACK NEEDS THE SAME MEMORY RELEASE ALWAYS HAD, and not having it was
     * the click. env_t starts at 0 and the ramp was `env = t`, so an attack
     * restarted from SILENCE wherever the envelope actually was: at the
     * boundary between two adjacent ON steps the gain went 1.000 -> 0.000 in
     * ONE SAMPLE, which is a click by definition. A gap resets the envelope
     * to 0 legitimately, so alternating on/off never showed it and a run of
     * consecutive ON steps clicked at every boundary -- which is what made it
     * read as intermittent. */
    float  att_from;      /* env level when ATTACK began */

    /* Published for the UI, computed once per block. Held here rather than
     * recomputed in get_param because get_param runs on the audio callback
     * too and must stay trivial. */
    float  ms_per_step;
    /* "THE PLAYHEAD IS MOVING". Equal to the transport state today, and kept
     * as its own name on purpose: the UI's extrapolator is the only reader
     * and is written against the concept, not against what drives it. It once
     * held `beats >= 0` while a free-run mode moved step_pos with the
     * transport stopped, and the playhead froze exactly where it was most
     * wanted. That mode is gone; the distinction is cheap to keep and the
     * lesson is not. */
    int    advancing;
} tg_instance_t;

static const host_api_v1_t *g_host = NULL;

/* ------------------------------------------------------------- helpers -- */

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline int pat_step_on(const tg_pattern_t *p, int i) {
    return (p->steps >> i) & 1u;
}

static inline int pat_step_tied(const tg_pattern_t *p, int i) {
    return (p->ties >> i) & 1u;
}

static int rate_index_from(const char *val) {
    if (!val || !*val) return TG_RATE_DEFAULT;
    for (int i = 0; i < TG_NUM_RATES; i++) {
        if (strcmp(val, tg_rates[i].label) == 0) return i;
    }
    /* A bare number is an index -- the host resolves a numeric enum value
     * that way, and an older state blob may carry one. */
    char *end = NULL;
    long n = strtol(val, &end, 10);
    if (end && end != val && n >= 0 && n < TG_NUM_RATES) return (int)n;
    return TG_RATE_DEFAULT;
}

/* Tiny JSON scanners. No allocation, no strtok, no locale surprises. */
static int json_get_number(const char *json, const char *key, double *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    *out = atof(p);
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_len - 1) out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

/* ------------------------------------------------------------ envelope -- */

static inline double ms_to_samples(float ms) {
    return (double)ms * (SAMPLE_RATE / 1000.0);
}

/*
 * Entering a stage, with zero-length stages walked THROUGH rather than
 * recursed through.
 *
 * This was env_enter calling env_settle calling env_enter: bounded in fact
 * (attack -> decay -> sustain is the longest chain a zero can open) but the
 * bound lived in two functions and a guard counter, so it read as unbounded.
 * A 0 ms attack must not spend a sample reporting env == 0 -- that is an
 * audible click at the step edge -- which is the whole reason the walk exists.
 */
static void env_enter(tg_instance_t *in, int stage) {
    for (;;) {
        in->env_stage = stage;
        in->env_t = 0.0;

        double len;
        switch (stage) {
        case TG_ATTACK:  len = ms_to_samples(in->attack_ms);
                         in->att_from = in->env;                      break;
        case TG_DECAY:   len = ms_to_samples(in->decay_ms);           break;
        case TG_RELEASE: len = ms_to_samples(in->release_ms);
                         in->rel_from = in->env;                      break;
        default:         len = 0.0;                                   break;
        }
        /* The one division a stage pays, taken once instead of per sample.
         * Guarded by the zero-length walk below, so it is only ever taken on
         * a positive length. */
        in->env_inc = (len > 0.0) ? (1.0 / len) : 0.0;

        /* SUSTAIN and IDLE have no length and are where the walk stops. */
        if (stage != TG_ATTACK && stage != TG_DECAY && stage != TG_RELEASE) return;
        if (len > 0.0) return;

        switch (stage) {
        case TG_ATTACK:  in->env = 1.0f;        stage = TG_DECAY;   break;
        case TG_DECAY:   in->env = in->sustain; stage = TG_SUSTAIN; break;
        default:         in->env = 0.0f;        stage = TG_IDLE;    break;
        }
    }
}

static inline void env_advance(tg_instance_t *in) {
    switch (in->env_stage) {
    case TG_ATTACK:
        /* FROM WHERE IT IS, not from zero -- the same thing RELEASE does with
         * rel_from. It still REACHES 1.0 and still takes attack_ms to get
         * there; it simply does not fall off a cliff first. */
        in->env = (float)(in->att_from + (1.0 - in->att_from) * in->env_t);
        if ((in->env_t += in->env_inc) >= 1.0) { in->env = 1.0f; env_enter(in, TG_DECAY); }
        break;
    case TG_DECAY:
        in->env = (float)(1.0 - (1.0 - in->sustain) * in->env_t);
        if ((in->env_t += in->env_inc) >= 1.0) { in->env = in->sustain; env_enter(in, TG_SUSTAIN); }
        break;
    case TG_SUSTAIN:
        in->env = in->sustain;
        break;
    case TG_RELEASE:
        in->env = (float)(in->rel_from * (1.0 - in->env_t));
        if ((in->env_t += in->env_inc) >= 1.0) { in->env = 0.0f; env_enter(in, TG_IDLE); }
        break;
    case TG_IDLE:
    default:
        in->env = 0.0f;
        break;
    }
}

/* A step boundary. This is the whole of what a tie means: an ON step arriving
 * on top of a held ON step does NOT restart the envelope. */
static void on_step_boundary(tg_instance_t *in, const tg_pattern_t *p,
                             int prev_step, int new_step) {
    int on_now  = pat_step_on(p, new_step);
    int on_prev = (prev_step >= 0) && pat_step_on(p, prev_step);
    int tied    = (prev_step >= 0) && pat_step_tied(p, prev_step);

    if (on_now) {
        if (!(on_prev && tied)) env_enter(in, TG_ATTACK);
    } else if (on_prev || in->env_stage != TG_IDLE) {
        env_enter(in, TG_RELEASE);
    }
}

/* ---------------------------------------------------------------- audio -- */

/*
 * PHASE IS ANCHORED PER BLOCK AND ADVANCED PER SAMPLE.
 *
 * get_beat_position() is interpolated per block and its intra-tick fraction is
 * CLAMPED to <= 1.0, so "a late tick freezes phase instead of overshooting and
 * snapping back" (shadow_transport.c). Differencing it per sample therefore
 * renders that plateau as an audible stutter on every late clock tick. Instead
 * the host's answer is used as an anchor that a local accumulator is pulled
 * gently towards -- a phase-locked loop, not a clock divider.
 */
#define TG_RESYNC_STEPS 0.25   /* beyond this, jump rather than glide */
#define TG_TRACK_GAIN   0.05   /* fraction of the error absorbed per block */

static void v2_process_block(void *instance, int16_t *audio_inout, int frames) {
    tg_instance_t *in = (tg_instance_t *)instance;
    if (!in || !audio_inout || frames <= 0) return;

    const tg_pattern_t *p = &in->pat[in->slot];
    int length = p->length;
    if (length < 1) length = 1;
    if (length > TG_MAX_STEPS) length = TG_MAX_STEPS;

    float bpm = 120.0f;
    if (g_host && g_host->get_bpm) {
        float b = g_host->get_bpm();
        if (b > 1.0f && b < 1000.0f) bpm = b;
    }

    double beats_per_step = tg_rates[in->rate_idx].beats;
    double samples_per_step = (60.0 / bpm) * SAMPLE_RATE * beats_per_step;
    if (samples_per_step < 1.0) samples_per_step = 1.0;
    double inc = 1.0 / samples_per_step;

    double beats = -1.0;
    if (g_host && g_host->get_beat_position) beats = g_host->get_beat_position();
    int running = (beats >= 0.0);

    /* What the UI needs to animate between reads -- see the `ui` readout.
     * It equals `running` now that free-run is gone, and keeps its own name
     * because the UI is written against the CONCEPT -- "is the playhead
     * moving" -- and must not have to know what makes it move. */
    in->ms_per_step = (float)(samples_per_step * 1000.0 / SAMPLE_RATE);
    in->advancing = running;

    if (running) {
        double target = beats / beats_per_step;
        if (!in->was_running) {
            /* Transport just started: land exactly, do not glide in. */
            in->step_pos = target;
            in->last_step = -1;
        } else {
            double err = target - in->step_pos;
            if (err > TG_RESYNC_STEPS || err < -TG_RESYNC_STEPS) {
                in->step_pos = target;       /* loop, seek or tempo jump */
                /*
                 * A JUMP RE-EVALUATES THE STEP, even when it lands on the same
                 * index. The boundary test is `step != last_step`, so a seek
                 * back onto the step we were already on fires nothing and the
                 * envelope keeps whatever state the old position left it in --
                 * typically released, so the step is silent until the pattern
                 * comes round again. Forgetting the last step makes the next
                 * sample a boundary and the gate re-reads the pattern.
                 */
                in->last_step = -1;
            } else {
                inc += (err * TG_TRACK_GAIN) / (double)frames;
            }
        }
    } else {
        /*
         * STOPPED MEANS OPEN. Hold the gate open and park at step 0, so the
         * next start is a downbeat rather than wherever the pattern happened
         * to stop.
         *
         * This used to be one of two modes, the other being a free-run that
         * accumulated step_pos with no transport to anchor against -- which is
         * why it needed an explicit fmod wrap to stop it walking off the end
         * of what a double indexes cleanly. With it gone, every path that
         * moves step_pos is re-anchored per block against an ABSOLUTE
         * beats/beats_per_step, so nothing is left that can grow unbounded.
         */
        in->step_pos = 0.0;
        in->last_step = -1;
        in->env_stage = TG_IDLE;
        in->env = 0.0f;
        in->was_running = 0;

        return;   /* passthrough: the buffer is already the dry signal */
    }
    in->was_running = running;

    /*
     * AMOUNT ZERO IS A TRUE BYPASS, so do not spend a block proving it.
     *
     * m = 1 - amount*(1 - env*level) collapses to exactly 1.0 when amount is
     * 0, i.e. every sample is multiplied by one and written back unchanged.
     * The PHASE is still advanced -- step_pos moves by the whole block below
     * -- so turning Amount back up lands on the step the pattern would have
     * reached rather than on the one it was left at, and the ring and the
     * pads keep sweeping while it is silent.
     *
     * The ENVELOPE is left frozen mid-stage, which is safe only because
     * attack now ramps from wherever env is: under the old zero-restart this
     * would have resumed with a full-scale step, i.e. the same click this
     * path looks like it could not possibly cause.
     */
    if (in->amount <= 0.0f) {
        in->step_pos += inc * (double)frames;
        return;   /* the buffer is already the dry signal */
    }

    /*
     * THE STEP INDEX IS CARRIED, NOT RECOMPUTED.
     *
     * It used to be `(int)floor(step_pos)` followed by `((step % length) +
     * length) % length` EVERY SAMPLE -- a double floor plus two integer
     * divisions to answer a question whose answer changes only at a step
     * boundary, i.e. a few times per block at most. Derived once here and
     * advanced by the same carry that advances the phase.
     *
     * `step_pos` itself still accumulates absolutely, because the synced path
     * re-anchors against an absolute beats/beats_per_step next block. This is
     * a cheaper way to READ it, not a replacement for it.
     */
    double frac = in->step_pos - floor(in->step_pos);
    int step = (int)fmod(floor(in->step_pos), (double)length);
    if (step < 0) step += length;

    for (int i = 0; i < frames; i++) {
        if (step != in->last_step) {
            on_step_boundary(in, p, in->last_step, step);
            in->last_step = step;
        }

        /*
         * GATE LENGTH: release inside the step, not only at its edge.
         *
         * Only for a step that is ON and is NOT tied into the next one -- a
         * tie means "hold through", so shortening it would contradict the tie
         * and make the two controls fight. Below 1.0 this is what shortens the
         * gate; at 1.0 the condition never fires and the envelope releases on
         * the boundary exactly as before.
         */
        if (in->hold < 1.0f && in->env_stage != TG_RELEASE && in->env_stage != TG_IDLE) {
            if (frac >= (double)in->hold &&
                !(pat_step_on(p, step) && pat_step_tied(p, step))) {
                env_enter(in, TG_RELEASE);
            }
        }

        env_advance(in);

        /*
         * TWO DEPTHS, AND THE ORDER IS THE POINT. The step's own level scales
         * how far THIS gate closes; the global depth then scales the whole
         * sequence. So a quiet step stays quiet relative to its neighbours as
         * you ride the global amount, which is what makes it an accent rather
         * than a second master.
         */
        /*
         * THE STEP'S AMOUNT IS HOW FAR THE GATE OPENS, NOT HOW FAR IT CLOSES.
         *
         * It used to scale the CLOSING: `1 - amount*level*(1-env)`. That reads
         * backwards at both ends. A level of 0 meant "this step is not gated",
         * so the audio passed at full -- setting a step to 0% made it LOUD --
         * and on a step that was on it did nothing at all, because a fully
         * open gate has nothing to close. Reported exactly that way: amount at
         * 0% and the step still sounding.
         *
         * The envelope is a gate opening 0..1, so a step's level is simply how
         * far it is allowed to open:
         *
         *     m = 1 - amount * (1 - env * level)
         *
         * level 1 is unchanged from before, level 0.5 is half as loud, level 0
         * is silent, and an OFF step is a gap whatever its level -- env is 0
         * there, so the term vanishes, which is right: a gap has no loudness.
         * The global `amount` still scales the whole effect as the dry/wet.
         */
        float level = (float)p->depth[step] * (1.0f / 255.0f);
        float m = 1.0f - in->amount * (1.0f - in->env * level);

        float l = (float)audio_inout[i * 2]     * m;
        float r = (float)audio_inout[i * 2 + 1] * m;
        audio_inout[i * 2]     = (int16_t)(l > 32767.0f ? 32767.0f : (l < -32768.0f ? -32768.0f : l));
        audio_inout[i * 2 + 1] = (int16_t)(r > 32767.0f ? 32767.0f : (r < -32768.0f ? -32768.0f : r));

        in->step_pos += inc;

        /*
         * `while`, not `if`: samples_per_step is clamped to >= 1.0 so `inc`
         * cannot exceed 1.0 today and one crossing per sample is all that can
         * happen -- but the PLL adds a per-block correction to `inc`, and a
         * loop that is correct for any positive increment costs a predicted
         * not-taken branch. The backward guard is the same argument for a
         * correction that momentarily outruns the increment.
         */
        frac += inc;
        while (frac >= 1.0) { frac -= 1.0; if (++step >= length) step = 0; }
        while (frac < 0.0)  { frac += 1.0; if (--step < 0) step = length - 1; }
    }
}

/* --------------------------------------------------------------- params -- */

static void pattern_defaults(tg_pattern_t *p, int slot) {
    p->length = 16;
    p->ties = 0;
    for (int i = 0; i < TG_MAX_STEPS; i++) p->depth[i] = TG_DEPTH_FULL;
    /* Slot 1 is every other step -- the plainest thing that is audibly a gate
     * the moment the module is loaded. The rest start fully open, which is
     * silence-free rather than "the effect is broken". */
    p->steps = (slot == 0) ? 0x5555u : 0xFFFFu;
}

static void *v2_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir; (void)config_json;
    tg_instance_t *in = (tg_instance_t *)calloc(1, sizeof(tg_instance_t));
    if (!in) return NULL;

    for (int s = 0; s < TG_SLOTS; s++) pattern_defaults(&in->pat[s], s);
    in->slot = 0;
    in->rate_idx = TG_RATE_DEFAULT;
    in->attack_ms = 2.0f;
    in->decay_ms = 20.0f;
    in->sustain = 1.0f;
    in->release_ms = 20.0f;
    in->hold = 1.0f;
    in->amount = 1.0f;

    in->last_step = -1;
    in->env_stage = TG_IDLE;
    return in;
}

static void v2_destroy_instance(void *instance) { free(instance); }

static void set_pattern_hex(uint32_t *dst, const char *val) {
    if (!val) return;
    *dst = (uint32_t)strtoul(val, NULL, 16);
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    tg_instance_t *in = (tg_instance_t *)instance;
    if (!in || !key || !val) return;
    tg_pattern_t *p = &in->pat[in->slot];

    if (strcmp(key, "slot") == 0) {
        int s = atoi(val) - 1;               /* shown 1..8, held 0..7 */
        if (s >= 0 && s < TG_SLOTS) {
            in->slot = s;
            /* The cursor is GLOBAL and the length is PER SLOT, so switching to
             * a shorter pattern can leave it past the end -- where every edit
             * lands on a step the ring never draws. The length branch below
             * clamps for the same reason; both doors need the same lock. */
            if (in->cursor >= in->pat[s].length) in->cursor = in->pat[s].length - 1;
            if (in->cursor < 0) in->cursor = 0;
        }
    } else if (strcmp(key, "length") == 0) {
        /* Option INDEX, as for `cursor`: index 15 is the option named "16",
         * which is a length of 16. */
        int n = atoi(val) + 1;
        p->length = n < 1 ? 1 : (n > TG_MAX_STEPS ? TG_MAX_STEPS : n);
        /* A cursor left beyond the new end would edit a step the ring does not
         * draw -- an invisible write. */
        if (in->cursor >= p->length) in->cursor = p->length - 1;
    } else if (strcmp(key, "rate") == 0) {
        in->rate_idx = rate_index_from(val);
    } else if (strcmp(key, "attack") == 0) {
        in->attack_ms = clampf((float)atof(val), 0.0f, 500.0f);
    } else if (strcmp(key, "decay") == 0) {
        in->decay_ms = clampf((float)atof(val), 0.0f, 500.0f);
    } else if (strcmp(key, "sustain") == 0) {
        in->sustain = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "hold") == 0) {
        in->hold = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "release") == 0) {
        in->release_ms = clampf((float)atof(val), 0.0f, 500.0f);
    } else if (strcmp(key, "amount") == 0) {
        in->amount = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "cursor") == 0) {
        /*
         * THE WIRE CARRIES THE OPTION INDEX, and the option NAMES carry the
         * step numbers -- so index 15 displays as "16".
         *
         * It used to send the 1-based name with `options_as_string`, which is
         * the other legal convention, and it displayed one too high: the host
         * has three resolvers for an enum's wire format and only two of them
         * consult that flag. formatParamValue treats the raw as an index
         * unconditionally, so a name of "16" was rendered as options[16] --
         * "17". Reported as the length knob reading 17 for a 16-step pattern.
         *
         * Indices are the host's default convention ("A NUMBER IS AN INDEX"),
         * so speaking them makes all three agree without depending on the
         * flag. It also makes this key agree with the `ui` readout, which was
         * already 0-based.
         */
        int c = atoi(val);
        if (c < 0) c = 0;
        if (c >= p->length) c = p->length - 1;
        in->cursor = c;
    } else if (strcmp(key, "step") == 0) {
        /*
         * THE ONE KNOB THAT EDITS THE PATTERN, and it is three-state rather
         * than two because a tie is not a separate property of a step -- it is
         * the third thing a step can be. Off / On / Tie maps exactly onto the
         * two bits, costs one knob instead of two, and cannot express the
         * meaningless fourth combination (tied while off).
         */
        int c = in->cursor;
        if (c >= 0 && c < TG_MAX_STEPS) {
            uint32_t bit = 1u << c;
            int mode;
            if (strcmp(val, "Off") == 0)      mode = 0;
            else if (strcmp(val, "On") == 0)  mode = 1;
            else if (strcmp(val, "Tie") == 0) mode = 2;
            else { mode = atoi(val); if (mode < 0) mode = 0; if (mode > 2) mode = 2; }

            if (mode == 0)      { p->steps &= ~bit; p->ties &= ~bit; }
            else if (mode == 1) { p->steps |=  bit; p->ties &= ~bit; }
            else                { p->steps |=  bit; p->ties |=  bit; }
        }
    } else if (strcmp(key, "step_amount") == 0) {
        int c = in->cursor;
        if (c >= 0 && c < TG_MAX_STEPS) {
            float f = clampf((float)atof(val), 0.0f, 1.0f);
            p->depth[c] = (uint8_t)(f * 255.0f + 0.5f);
        }
    } else if (strcmp(key, "pattern") == 0) {
        set_pattern_hex(&p->steps, val);
    } else if (strcmp(key, "ties") == 0) {
        set_pattern_hex(&p->ties, val);
    } else if (strcmp(key, "state") == 0) {
        double n;
        /*
         * SIZED FROM THE FORMAT, NOT GUESSED.
         *
         * This was char[40] while the per-slot value it has to hold is
         * "<steps>:<ties>:<length>:<64 hex digits>" -- 85 characters at worst.
         * json_get_string truncates at the buffer, so roughly the first
         * fourteen per-step amounts survived a save and every later one came
         * back full. Nothing reported it: the pattern and the ring both looked
         * exactly right, and the round-trip test passed because it happened to
         * check step 4.
         *
         * TG_STATE_FIELD_MAX is derived so that widening the pattern cannot
         * quietly reintroduce it.
         */
        char sv[TG_STATE_FIELD_MAX];
        if (json_get_number(val, "slot", &n) == 0 && n >= 0 && n < TG_SLOTS) in->slot = (int)n;
        if (json_get_string(val, "rate", sv, sizeof(sv)) == 0) {
            in->rate_idx = rate_index_from(sv);
        } else if (json_get_number(val, "rate", &n) == 0) {
            /* A numeric rate is an INDEX, and must be resolved as one. Passing
             * "" here instead silently reset every such blob to the default --
             * a patch that loads, reports a rate, and runs at another. */
            char idx[16];
            snprintf(idx, sizeof(idx), "%d", (int)n);
            in->rate_idx = rate_index_from(idx);
        }
        if (json_get_number(val, "attack",  &n) == 0) in->attack_ms  = clampf((float)n, 0.0f, 500.0f);
        if (json_get_number(val, "decay",   &n) == 0) in->decay_ms   = clampf((float)n, 0.0f, 500.0f);
        if (json_get_number(val, "sustain", &n) == 0) in->sustain    = clampf((float)n, 0.0f, 1.0f);
        if (json_get_number(val, "release", &n) == 0) in->release_ms = clampf((float)n, 0.0f, 500.0f);
        /* Absent in v1 and v2 blobs, where the gate always ran the whole step;
         * 1.0 is that behaviour, so an old patch is unchanged. */
        in->hold = 1.0f;
        if (json_get_number(val, "hold", &n) == 0) in->hold = clampf((float)n, 0.0f, 1.0f);
        /*
         * THREE SPELLINGS OF ONE VALUE, AND THE OLD PAIR MULTIPLIES.
         *
         * v3 writes `amount`. v2 wrote `mix` and `depth`, which spanned one
         * degree of freedom between them -- only their product ever reached
         * the audio -- so the faithful migration is that product. Reading just
         * one of the two would make every patch saved with depth < 1 jump to
         * full, which is louder gating on a patch that had been working.
         */
        if (json_get_number(val, "amount", &n) == 0) {
            in->amount = clampf((float)n, 0.0f, 1.0f);
        } else {
            double legacy_mix = 1.0, legacy_depth = 1.0;
            int saw = 0;
            if (json_get_number(val, "mix", &legacy_mix) == 0) saw = 1;
            if (json_get_number(val, "depth", &legacy_depth) == 0) saw = 1;
            if (saw) {
                in->amount = clampf((float)(clampf((float)legacy_mix, 0.0f, 1.0f) *
                                            clampf((float)legacy_depth, 0.0f, 1.0f)),
                                    0.0f, 1.0f);
            }
        }

        /* Patterns travel as one array of "steps:ties:length" triples so a
         * slot cannot be restored half-applied. */
        for (int s = 0; s < TG_SLOTS; s++) {
            char pk[8];
            snprintf(pk, sizeof(pk), "p%d", s);
            if (json_get_string(val, pk, sv, sizeof(sv)) == 0) {
                unsigned st = 0, ti = 0; int len = 16, used = 0;
                if (sscanf(sv, "%x:%x:%d%n", &st, &ti, &len, &used) == 3) {
                    in->pat[s].steps  = (uint32_t)st;
                    in->pat[s].ties   = (uint32_t)ti;
                    in->pat[s].length = len < 1 ? 1 : (len > TG_MAX_STEPS ? TG_MAX_STEPS : len);

                    /*
                     * A V1 TRIPLE HAS NO DEPTHS, AND ABSENT MEANS FULL.
                     *
                     * Every patch saved before this version ends after the
                     * length. Leaving the array at whatever the struct held --
                     * or zeroing it -- would load those patches silent, with
                     * the pattern and the ring both looking completely
                     * correct. This is the whole reason the state version
                     * exists; see the Ducker's `sv` comment.
                     */
                    for (int i = 0; i < TG_MAX_STEPS; i++)
                        in->pat[s].depth[i] = TG_DEPTH_FULL;

                    const char *d = sv + used;
                    if (*d == ':') {
                        d++;
                        for (int i = 0; i < TG_MAX_STEPS; i++) {
                            if (!isxdigit((unsigned char)d[0]) ||
                                !isxdigit((unsigned char)d[1])) break;
                            char pair[3] = { d[0], d[1], 0 };
                            in->pat[s].depth[i] = (uint8_t)strtoul(pair, NULL, 16);
                            d += 2;
                        }
                    }
                }
            }
        }
    }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    tg_instance_t *in = (tg_instance_t *)instance;
    if (!in || !key) return -1;
    const tg_pattern_t *p = &in->pat[in->slot];

    if (strcmp(key, "name") == 0)    return snprintf(buf, buf_len, "TRANCE GATE");
    if (strcmp(key, "slot") == 0)    return snprintf(buf, buf_len, "%d", in->slot + 1);
    if (strcmp(key, "length") == 0)  return snprintf(buf, buf_len, "%d", p->length - 1);
    if (strcmp(key, "rate") == 0)    return snprintf(buf, buf_len, "%s", tg_rates[in->rate_idx].label);
    if (strcmp(key, "attack") == 0)  return snprintf(buf, buf_len, "%.1f", in->attack_ms);
    if (strcmp(key, "decay") == 0)   return snprintf(buf, buf_len, "%.1f", in->decay_ms);
    if (strcmp(key, "sustain") == 0) return snprintf(buf, buf_len, "%.2f", in->sustain);
    if (strcmp(key, "release") == 0) return snprintf(buf, buf_len, "%.1f", in->release_ms);
    if (strcmp(key, "hold") == 0)    return snprintf(buf, buf_len, "%.2f", in->hold);
    if (strcmp(key, "amount") == 0)  return snprintf(buf, buf_len, "%.2f", in->amount);
    if (strcmp(key, "cursor") == 0) return snprintf(buf, buf_len, "%d", in->cursor);
    if (strcmp(key, "step") == 0) {
        uint32_t bit = 1u << in->cursor;
        const char *w = !(p->steps & bit) ? "Off" : ((p->ties & bit) ? "Tie" : "On");
        return snprintf(buf, buf_len, "%s", w);
    }
    if (strcmp(key, "step_amount") == 0)
        return snprintf(buf, buf_len, "%.2f", p->depth[in->cursor] * (1.0f / 255.0f));
    if (strcmp(key, "pattern") == 0) return snprintf(buf, buf_len, "%X", p->steps);
    if (strcmp(key, "ties") == 0)    return snprintf(buf, buf_len, "%X", p->ties);

    /* The live playhead. Declared "live": true, so the host re-reads
     * `phase:effective` every tick instead of once per value rotation -- the
     * difference between an animated ring and a slideshow. Fractional, so the
     * UI can put the marker between two segments.
     *
     * `:effective` is served identically: the chain host strips the suffix for
     * a key it is not modulating, so a module that answers only the plain key
     * is simply never asked. */
    if (strcmp(key, "phase") == 0 || strcmp(key, "phase:effective") == 0) {
        int length = p->length < 1 ? 1 : p->length;
        double pos = fmod(in->step_pos, (double)length);
        if (pos < 0) pos += length;
        return snprintf(buf, buf_len, "%.3f", pos);
    }

    /*
     * ONE READ FOR THE WHOLE PICTURE.
     *
     * The animated page needs six facts and a param read is ~2.8 ms, so asking
     * for them separately would cost six rotation stops -- and an `extra_keys`
     * value is refreshed on the SLOW rotation only (refreshModulatedValues
     * walks page.keys, which an extra key is deliberately not in), so six keys
     * would be six times slower than one, not merely six reads.
     *
     * `ms_step` is the half that makes the animation smooth regardless: with
     * it the page can advance the playhead locally at frame rate and only
     * re-anchor when a fresh read lands. Without it the playhead can only move
     * as often as the rotation comes round, which is the slideshow the user
     * sees.
     *
     *   steps : ties : length : phase : ms_step : advancing : cursor : depths
     */
    if (strcmp(key, "ui") == 0) {
        int length = p->length < 1 ? 1 : p->length;
        double pos = fmod(in->step_pos, (double)length);
        if (pos < 0) pos += length;
        int n = snprintf(buf, buf_len, "%X:%X:%d:%.3f:%.2f:%d:%d:",
                         p->steps, p->ties, length, pos,
                         in->ms_per_step, in->advancing, in->cursor);
        /* Per-step depths as a run of two hex digits each -- one field rather
         * than 32, because the page already pays for this string once per
         * rotation stop and a second read would halve the anchor rate. */
        for (int i = 0; i < length && n > 0 && n + 2 < buf_len; i++) {
            static const char HEX[] = "0123456789ABCDEF";
            buf[n++] = HEX[(p->depth[i] >> 4) & 0xF];
            buf[n++] = HEX[p->depth[i] & 0xF];
        }
        if (n < buf_len) buf[n] = '\0';
        return n;
    }

    if (strcmp(key, "state") == 0) {
        int n = snprintf(buf, buf_len,
            "{\"sv\":%d,\"slot\":%d,\"rate\":\"%s\","
            "\"attack\":%.2f,\"decay\":%.2f,\"sustain\":%.3f,\"release\":%.2f,"
            "\"hold\":%.3f,\"amount\":%.3f",
            TG_STATE_VERSION, in->slot, tg_rates[in->rate_idx].label,
            in->attack_ms, in->decay_ms, in->sustain, in->release_ms,
            in->hold, in->amount);
        for (int s = 0; s < TG_SLOTS && n > 0 && n < buf_len; s++) {
            n += snprintf(buf + n, buf_len - n, ",\"p%d\":\"%X:%X:%d:",
                          s, in->pat[s].steps, in->pat[s].ties, in->pat[s].length);
            for (int i = 0; i < TG_MAX_STEPS && n > 0 && n + 2 < buf_len; i++) {
                n += snprintf(buf + n, buf_len - n, "%02X", in->pat[s].depth[i]);
            }
            if (n > 0 && n < buf_len) n += snprintf(buf + n, buf_len - n, "\"");
        }
        if (n > 0 && n < buf_len) n += snprintf(buf + n, buf_len - n, "}");
        return (n >= buf_len) ? -1 : n;
    }

    /* Refusing ui_hierarchy is what routes the component editor to our
     * ui_chain.js, which is the only route to host_pad_block and therefore to
     * pad editing. enterComponentEdit() tries the hierarchy editor first and
     * returns; only the null fallback reaches loadModuleUi. */
    /*
     * SERVED-EMPTY, NOT REFUSED.
     *
     * Returning -1 reads as "the read did not complete", and the component
     * entry gate then HOLDS for HOLD_UNSERVED_READ_LIMIT attempts before
     * falling back (component_load_gate.mjs) -- a delay on every entry, for a
     * question we can answer instantly. "" means "I have none", which is the
     * truth: the hierarchy is supplied by ui_chain.js to its own controller.
     *
     * Both spellings are falsy, so getComponentHierarchy still returns null
     * and ui_chain.js still loads. Only the wait goes away.
     */
    if (strcmp(key, "ui_hierarchy") == 0) { if (buf_len > 0) buf[0] = '\0'; return 0; }

    if (strcmp(key, "chain_params") == 0) {
        /* THIS copy is the one the UI reads -- the chain host asks the plugin
         * first and only falls back to re-parsing module.json, which drops
         * `viz` entirely. Declared here rather than in module.json for a
         * second reason: module.json is read by a minimal parser with an 8 KB
         * budget and this does not comfortably fit.
         *
         * The four envelope keys are CONTIGUOUS on purpose: a viz group whose
         * members do not land on one row of the 2x4 page is dropped whole,
         * silently. */
        static const char *params =
        "["
        /*
         * `options_as_string` because this one speaks NAMES -- get answers
         * `slot + 1`, set does `atoi(val) - 1` -- while `length` and `cursor`
         * below speak INDICES off an option list that looks identical.
         *
         * Two numeral enums, opposite conventions, and NEITHER can be worked
         * out from a value: every index that is at least 1 is also one of the
         * option names, so "1" is both slot 0 by name and slot 1 by number.
         * The host's learner used to guess (name first) and was right here by
         * luck and wrong on `length`, which is how a 16-step pattern came to
         * read 15 and write 17. It refuses to guess now, so an ambiguous enum
         * that does not declare is read as an index -- which is correct for
         * the two below and would silently shift this one by a slot.
         */
        "{\"key\":\"slot\",\"name\":\"Slot\",\"type\":\"enum\","
          "\"options_as_string\":true,"
          "\"options\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\"],\"default\":\"1\"},"
        /* Knob 3: this step's amount -- an accent, scaled by the global
         * Amount on the settings page. Same quantity, two scopes, which is why
         * they share a name, a unit and a range. */
        "{\"key\":\"step_amount\",\"name\":\"Step Amount\",\"short_name\":\"Step\",\"type\":\"float\",\"min\":0,\"max\":1,"
          "\"default\":1,\"step\":0.01,\"unit\":\"%\"},"
        /* Same reasoning as `cursor`: 32 values at one detent each is a flick
         * from end to end. */
        "{\"key\":\"length\",\"name\":\"Len\",\"type\":\"enum\","
          "\"options\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\","
           "\"11\",\"12\",\"13\",\"14\",\"15\",\"16\",\"17\",\"18\",\"19\",\"20\","
           "\"21\",\"22\",\"23\",\"24\",\"25\",\"26\",\"27\",\"28\",\"29\",\"30\","
           "\"31\",\"32\"],\"wire_format\":\"index\",\"default\":\"15\"},"
        /* A PLAIN ENUM, NOT type "rate".
         *
         * `rate` is expanded into its option list by buildRateParamMeta in
         * shadow_ui.js -- HOST code. This module builds its own controller out
         * of the shared param_pages library, which has never heard of the type,
         * so the meta arrived as {type:"rate"} with no options, no min and no
         * max: undiscretised, and the knob could not step through note values
         * at all. Declaring the options here is what makes the knob discrete,
         * and it costs nothing else -- the labels ARE the wire value either
         * way, so the same C table answers both. */
        "{\"key\":\"rate\",\"name\":\"Rate\",\"type\":\"enum\",\"options\":"
          "[\"1/1T\",\"1/2\",\"1/2T\",\"1/4\",\"1/4T\",\"1/8\",\"1/8T\","
           "\"1/16\",\"1/16T\",\"1/32\",\"1/32T\",\"1/64\"],"
          "\"default\":\"1/16\"},"
        /*
         * Knob 1: which step the ring's marker sits on.
         *
         * AN ENUM, NOT AN INT, AND THE REASON IS THE FEEL.
         * knob_engine gives an int ONE detent per value unless its range is
         * 2..16 (NARROW_RANGE_MAX), where it gives four. A step cursor is a
         * discrete identity exactly like the pad index that constant was
         * raised for -- "these numbers move crazy fast on a single detent" --
         * but 1..32 falls outside the band, so it flew past the whole ring on
         * one flick. knobStep divides an ENUM's delta by ENUM_DELTA_DIV (4)
         * whatever its length, which is the feel we want and the house
         * constant rather than a number invented here.
         *
         * The options are numerals, so a value is ambiguous between a name
         * and an index -- and the host's THREE enum resolvers do not agree
         * about which to prefer. Speaking the index is what they all read the
         * same way; see the note in set_param.
         */
        "{\"key\":\"cursor\",\"name\":\"Step\",\"type\":\"enum\","
          "\"options\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\","
           "\"11\",\"12\",\"13\",\"14\",\"15\",\"16\",\"17\",\"18\",\"19\",\"20\","
           "\"21\",\"22\",\"23\",\"24\",\"25\",\"26\",\"27\",\"28\",\"29\",\"30\","
           "\"31\",\"32\"],\"wire_format\":\"index\",\"default\":\"0\"},"
        /* Knob 2: what the step under the cursor IS. Three states, not a
         * switch plus a tie switch -- see set_param. */
        "{\"key\":\"step\",\"name\":\"Gate\",\"short_name\":\"Gate\",\"type\":\"enum\","
          "\"options\":[\"Off\",\"On\",\"Tie\"],\"default\":\"Off\"},"
        "{\"key\":\"attack\",\"name\":\"Att\",\"type\":\"float\",\"min\":0,\"max\":500,"
          "\"default\":2,\"step\":1,\"unit\":\"ms\","
          "\"viz\":{\"group\":\"adsr\",\"role\":\"attack\",\"kind\":\"envelope\"}},"
        "{\"key\":\"decay\",\"name\":\"Dec\",\"type\":\"float\",\"min\":0,\"max\":500,"
          "\"default\":20,\"step\":1,\"unit\":\"ms\","
          "\"viz\":{\"group\":\"adsr\",\"role\":\"decay\"}},"
        "{\"key\":\"sustain\",\"name\":\"Sus\",\"type\":\"float\",\"min\":0,\"max\":1,"
          "\"default\":1,\"step\":0.01,\"unit\":\"%\","
          "\"viz\":{\"group\":\"adsr\",\"role\":\"sustain\"}},"
        "{\"key\":\"release\",\"name\":\"Rel\",\"type\":\"float\",\"min\":0,\"max\":500,"
          "\"default\":20,\"step\":1,\"unit\":\"ms\","
          "\"viz\":{\"group\":\"adsr\",\"role\":\"release\"}},"
        /*
         * THE SAME WORD, AND THE SCOPE IN FRONT OF IT.
         *
         * These are one quantity at two scopes, so they share the word -- but
         * naming them identically made them indistinguishable on the grid, and
         * the per-step one was turned to zero expecting the whole effect to
         * bypass. It only silenced one step, which is what it is for. "Step"
         * and "All" say which you are holding; the shared "Amount" still says
         * they are the same idea.
         *
         * Shown 0-100%: `unit: "%"` with max 1 is what makes the formatter
         * scale it. Zero here is a true bypass -- see the gain in
         * process_block.
         */
        /* Gate length. Sustain is a LEVEL and has none; this is the duration
         * control, expressed as a share of the step so it follows the Rate. */
        "{\"key\":\"hold\",\"name\":\"Gate\",\"short_name\":\"Gate\",\"type\":\"float\","
          "\"min\":0.05,\"max\":1,\"default\":1,\"step\":0.01,\"unit\":\"%\"},"
        "{\"key\":\"amount\",\"name\":\"All Amount\",\"short_name\":\"All\",\"type\":\"float\",\"min\":0,\"max\":1,"
          "\"default\":1,\"step\":0.01,\"unit\":\"%\"},"
        "{\"key\":\"pattern\",\"name\":\"Pat\",\"type\":\"string\",\"access\":\"read\"},"
        "{\"key\":\"ties\",\"name\":\"Ties\",\"type\":\"string\",\"access\":\"read\"},"
        "{\"key\":\"phase\",\"name\":\"Phase\",\"type\":\"string\",\"access\":\"read\","
          "\"live\":true},"
        /* The compound readout the animated page reads. It earns no cell --
         * nothing lists it in `knobs` -- so it needs no hiding. */
        "{\"key\":\"ui\",\"name\":\"UI\",\"type\":\"string\",\"access\":\"read\"},"
        /* THE RING. `as_page` makes this a page in the level's jog rotation
         * rather than a cell you dive into, carrying that level's own eight
         * knobs -- so the encoders work with no input code and the host draws
         * its own header, bank bar and footer AROUND the body. That last part
         * is what makes it impossible for the picture to collide with the
         * text: the drawer is handed a frame, not the screen. */
        "{\"key\":\"gate\",\"name\":\"Gate\",\"type\":\"canvas\",\"as_page\":true,"
          /* READ WITHOUT A KNOB. The rotation fetches page_knobs + extra_keys
           * and nothing else, so a value the drawer reads and the page does
           * not declare is simply absent and its label renders EMPTY -- no
           * error. `rate` used to arrive only because the canvas page took
           * the level's first eight knobs; page_knobs replaced that list and
           * took the Rate label out with it. */
          "\"show_value\":false,\"extra_keys\":[\"ui\",\"rate\"],"
          /* THE RING PAGE'S OWN KNOBS, which are not the grid's.
           *
           * Without this a canvas page takes the level's first eight knobs, so
           * the picture page and the grid behind it are the SAME EIGHT KEYS and
           * neither can be arranged without deranging the other. They want
           * different things: while you are looking at the pattern you reach
           * for the slot, the two amounts and the envelope; Length and Rate are
           * settings you leave alone. Those two live on the grid only, and
           * `slot` and `step_amount` live here only. */
          "\"page_knobs\":[\"slot\",\"amount\",\"step_amount\","
            "\"attack\",\"decay\",\"sustain\",\"release\",\"hold\"]}"
        "]";
        int len = (int)strlen(params);
        if (len >= buf_len) return -1;
        memcpy(buf, params, len + 1);
        return len;
    }

    return -1;
}

/* ----------------------------------------------------------------- midi -- */

/*
 * A chain-slot audio FX is handed system-realtime bytes as ONE-BYTE messages
 * on cable 0, independent of the user's MIDI-Clock-Out setting
 * (schwung_shim.c). Master FX is not -- it only ever sees pad notes -- so this
 * is an optimisation for the slot case, never the primary clock. Phase still
 * comes from get_beat_position(); this only removes the up-to-one-block lag
 * between pressing Stop and the gate opening.
 */
static void tg_on_midi(tg_instance_t *in, const uint8_t *msg, int len) {
    if (!in || len < 1) return;
    switch (msg[0]) {
    case 0xFA:                       /* Start: rewind to step 1 */
        in->step_pos = 0.0;
        in->last_step = -1;
        in->was_running = 0;         /* next block anchors hard */
        break;
    case 0xFC:                       /* Stop: open the gate */
        in->env_stage = TG_IDLE;
        in->env = 0.0f;
        in->last_step = -1;
        in->was_running = 0;
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- init -- */

static audio_fx_api_v2_t g_fx_api_v2;

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    memset(&g_fx_api_v2, 0, sizeof(g_fx_api_v2));
    g_fx_api_v2.api_version   = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance  = v2_create_instance;
    g_fx_api_v2.destroy_instance = v2_destroy_instance;
    g_fx_api_v2.process_block    = v2_process_block;
    g_fx_api_v2.set_param        = v2_set_param;
    g_fx_api_v2.get_param        = v2_get_param;
    /* on_midi is deliberately NOT set in the struct (ABI safety for hosts with
     * a shorter one). Both hosts discover it by dlsym'ing the symbol below. */
    return &g_fx_api_v2;
}

/* Chain host and Master FX both dlsym THIS, never the vtable field. */
void move_audio_fx_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    tg_on_midi((tg_instance_t *)instance, msg, len);
}
