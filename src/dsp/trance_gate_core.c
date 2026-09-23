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
#include "trance_gate_core.h"


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
#define TG_STATE_FIELD_EMIT (32 + 1 + 32 + 1 + 3 + 1 + 2 * TG_MAX_STEPS)
/* The buffer that READS one back, with room for the terminator and slack. */
#define TG_STATE_FIELD_MAX  (TG_STATE_FIELD_EMIT + 16)

/*
 * TWO BUDGETS, AND FOR A LONG TIME THIS GUARDED THE WRONG ONE.
 *
 *   MAX_FX_STATE_LEN      8192   a normal audio FX slot -- where this runs
 *   MAX_BUS_FX_STATE_LEN  1024   a bus insert
 *
 * The assert was pinned to 1024, which made 128 steps look impossible: eight
 * slots of them is ~2800 bytes. It is not impossible, it simply does not fit
 * a BUS INSERT, and the slot budget it actually uses has eight times the room.
 *
 * The 1024 case still matters because an oversized blob is dropped rather
 * than truncated (chain_internal.h) -- a patch would load with default
 * patterns and nothing would say why. What keeps that off the table in
 * practice is the trailing-default depth omission in the emitter: a pattern
 * with no per-step accents writes no depths, so eight slots of ordinary
 * patterns stay far inside 1024 even at full length. tests/test_core.c pins
 * where the boundary actually falls rather than leaving it to be discovered.
 */
/* 144 at worst today (every field at its longest, "1/16T" as the rate);
 * raised from 160 when "tmode" was added so the margin stays a margin rather
 * than something to recount on the next field. */
#define TG_STATE_HEADER_MAX 192
#define TG_STATE_WORST_CASE \
    (TG_STATE_HEADER_MAX + TG_SLOTS * (sizeof(",\"p0\":\"\"") + TG_STATE_FIELD_EMIT))
_Static_assert(TG_STATE_WORST_CASE <= 8192,
               "state blob can exceed an audio FX slot's 8192-byte cap; "
               "shrink the encoding, TG_MAX_STEPS or TG_SLOTS");
/* The number embedders size their buffers from is the PUBLIC half of the
 * same fact, so it is checked against the real encoding rather than kept in
 * step by hand. */
_Static_assert(TG_STATE_WORST_CASE < TG_STATE_MAX,
               "TG_STATE_MAX (trance_gate_core.h) is too small for the "
               "encoding; every embedder's get_param buffer would truncate");

enum { TG_IDLE = 0, TG_ATTACK, TG_DECAY, TG_SUSTAIN, TG_RELEASE };

/* What the envelope's three time values mean; see stage_samples. */
enum { TG_TIME_MS = 0, TG_TIME_PCT = 1 };

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
    /* APPENDED, never inserted: TG_RATE_DEFAULT is an index into this table
     * and so is the numeric form a state blob may carry, so putting 1/128
     * anywhere but the end would silently re-point every saved patch. */
    { "1/128", 0.03125    },
};
#define TG_NUM_RATES ((int)(sizeof(tg_rates) / sizeof(tg_rates[0])))
#define TG_RATE_DEFAULT 7

typedef struct {
    tg_mask_t steps;  /* bit i set = step i is ON                      */
    tg_mask_t ties;   /* bit i set = step i holds through into step i+1 */
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
    /*
     * THE STRUCK STEP'S LEVEL, HELD FOR THE WHOLE GATE.
     *
     * This used to be read fresh every sample as depth[current_step], which
     * is wrong in the two places where a gate and a step are not the same
     * span:
     *
     *   a RELEASE that runs past the step edge was scaled by the NEXT step's
     *   amount, so pulling a pad down to 50% quietened the body of its
     *   envelope and left the tail at whatever came after it;
     *
     *   a TIE holds one gate across a boundary, and the level stepped
     *   mid-gate -- a discontinuity in the gain, which is a click.
     *
     * Latched when the envelope enters ATTACK -- the moment a gate opens --
     * and held until it returns to IDLE. One gate, one level. A level contour
     * is what NOT tying gives you.
     */
    float  step_level;

    /* Published for the UI, computed once per block. Held here rather than
     * recomputed in get_param because get_param runs on the audio callback
     * too and must stay trivial. */
    float  ms_per_step;
    float  last_bpm;      /* the tempo the last block ran at; see set_rate_ms */
    /* "THE PLAYHEAD IS MOVING". Equal to the transport state today, and kept
     * as its own name on purpose: the UI's extrapolator is the only reader
     * and is written against the concept, not against what drives it. It once
     * held `beats >= 0` while a free-run mode moved step_pos with the
     * transport stopped, and the playhead froze exactly where it was most
     * wanted. That mode is gone; the distinction is cheap to keep and the
     * lesson is not. */
    int    advancing;
    /* Adjacent ON steps hold as ONE gate instead of re-articulating -- every
     * such pair behaves as if it were tied. See on_step_boundary. */
    int    legato;
    /* TG_TIME_MS or TG_TIME_PCT -- what attack/decay/release MEAN. See
     * stage_samples. */
    int    time_mode;
    /* The shell's, not a constant. Changing it only rescales derived lengths,
     * so it is safe to set from prepareToPlay. */
    double sample_rate;
} tg_instance_t;

/* ------------------------------------------------------------- helpers -- */

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline int pat_step_on(const tg_pattern_t *p, int i) {
    return tg_mask_get(&p->steps, i);
}

static inline int pat_step_tied(const tg_pattern_t *p, int i) {
    return tg_mask_get(&p->ties, i);
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

/* Per-instance now: a DAW runs at 48k or 96k, where a hardcoded 44100 makes
 * a 1/16 step 8.8% or 118% too long. */
static inline double ms_to_samples(const tg_instance_t *in, float ms) {
    return (double)ms * (in->sample_rate / 1000.0);
}

/*
 * HOW LONG A STAGE LASTS, in the unit the patch is written in.
 *
 * TG_TIME_MS   the value IS milliseconds. Absolute: the same envelope is a
 *              gentle swell at 1/4 and is never finished at 1/32.
 * TG_TIME_PCT  the value is 0..500 read as 0..100% OF THE STEP, so the shape
 *              survives a change of rate or tempo intact. The scale is shared
 *              with ms on purpose -- 250 is "250 ms" or "50%" depending only
 *              on the mode, so switching modes never moves a knob.
 *
 * ms_per_step is maintained by tg_block_setup and seeded at construction, so
 * it is correct here before any audio has run.
 */
static inline double stage_samples(const tg_instance_t *in, float value) {
    if (in->time_mode != TG_TIME_PCT) return ms_to_samples(in, value);
    return (double)value * (1.0f / 500.0f)
         * (double)in->ms_per_step * (in->sample_rate / 1000.0);
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
        case TG_ATTACK:  len = stage_samples(in, in->attack_ms);
                         in->att_from = in->env;                      break;
        case TG_DECAY:   len = stage_samples(in, in->decay_ms);           break;
        case TG_RELEASE: len = stage_samples(in, in->release_ms);
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
        /*
         * LEGATO IS "TREAT EVERY ADJACENT PAIR AS TIED".
         *
         * At Sustain 100% this changes nothing audible, because attack already
         * ramps from wherever the envelope is and there is nowhere to ramp
         * from a fully open gate. The difference appears BELOW 100%, which is
         * where a retrigger actually re-articulates -- worth knowing before
         * concluding the switch does nothing.
         */
        if (!(on_prev && (tied || in->legato))) {
            /* A GATE IS OPENING, so this step's amount becomes the gate's for
             * as long as it lives -- including a release that outlives the
             * step. Latched HERE and not inside env_enter because only the
             * boundary knows which step struck; env_enter is also reached
             * from the zero-length stage walk and from RELEASE, neither of
             * which starts a gate. */
            in->step_level = (float)p->depth[new_step] * (1.0f / 255.0f);
            env_enter(in, TG_ATTACK);
        }
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

/*
 * ONE SET OF MATHS, THREE BUFFER FORMATS.
 *
 * Move hands us int16 interleaved; VST3 and AU hand us float, usually as
 * separate channel pointers. Writing the loop three times would mean three
 * places for the gain law to drift, and the drift would be inaudible until
 * somebody A/B'd the plugin against the hardware. So the block setup and the
 * per-sample gain live here once, and the format-specific part is the two
 * lines that actually touch the buffer.
 */
typedef struct {
    const tg_pattern_t *p;
    int    length;
    double inc;
    double frac;
    int    step;
} tg_run_t;

/* Returns 0 when the block needs no gain applied at all -- a stopped
 * transport or a zero Amount -- having already advanced whatever state the
 * next block depends on. The caller leaves the buffer untouched, which is
 * exactly right: the dry signal is already in it. */
static int tg_block_setup(tg_instance_t *in, int frames,
                          const tg_transport_t *t, tg_run_t *r) {
    const tg_pattern_t *p = &in->pat[in->slot];
    int length = p->length;
    if (length < 1) length = 1;
    if (length > TG_MAX_STEPS) length = TG_MAX_STEPS;

    float bpm = 120.0f;
    if (t && t->bpm > 1.0f && t->bpm < 1000.0f) bpm = t->bpm;

    double beats_per_step = tg_rates[in->rate_idx].beats;
    double samples_per_step = (60.0 / bpm) * in->sample_rate * beats_per_step;
    if (samples_per_step < 1.0) samples_per_step = 1.0;
    double inc = 1.0 / samples_per_step;

    /* A stopped transport is not beat 0, it is no beat at all. */
    double beats = (t && t->running) ? t->beats : -1.0;
    int running = (beats >= 0.0);

    in->last_bpm = bpm;
    in->ms_per_step = (float)(samples_per_step * 1000.0 / in->sample_rate);
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
                /* A jump re-evaluates the step even when it lands on the same
                 * index: the boundary test is `step != last_step`, so a seek
                 * back onto the step we were already on would fire nothing and
                 * leave the envelope wherever the old position left it. */
                in->last_step = -1;
            } else {
                inc += (err * TG_TRACK_GAIN) / (double)frames;
            }
        }
    } else {
        /* Stopped means open: hold the gate open and park at step 0, so the
         * next start is a downbeat rather than wherever the pattern stopped. */
        in->step_pos = 0.0;
        in->last_step = -1;
        in->env_stage = TG_IDLE;
        in->env = 0.0f;
        in->was_running = 0;
        return 0;
    }
    in->was_running = running;

    /* Amount zero is a true bypass -- m collapses to exactly 1.0 -- so do not
     * spend a block proving it. The phase still advances, so turning it back
     * up lands on the step the pattern would have reached. */
    if (in->amount <= 0.0f) {
        in->step_pos += inc * (double)frames;
        return 0;
    }

    r->p = p;
    r->length = length;
    r->inc = inc;
    r->frac = in->step_pos - floor(in->step_pos);
    r->step = (int)fmod(floor(in->step_pos), (double)length);
    if (r->step < 0) r->step += length;
    return 1;
}

/* The gain for one sample, advancing every bit of state that depends on it. */
static inline float tg_next_gain(tg_instance_t *in, tg_run_t *r) {
    const tg_pattern_t *p = r->p;

    if (r->step != in->last_step) {
        on_step_boundary(in, p, in->last_step, r->step);
        in->last_step = r->step;
    }

    /* Gate length: release inside the step, not only at its edge. A tie means
     * "hold through", so shortening it would contradict the tie. */
    if (in->hold < 1.0f && in->env_stage != TG_RELEASE && in->env_stage != TG_IDLE) {
        if (r->frac >= (double)in->hold &&
            !(pat_step_on(p, r->step) && pat_step_tied(p, r->step))) {
            env_enter(in, TG_RELEASE);
        }
    }

    env_advance(in);

    /* The step's amount is how far the gate OPENS, not how far it closes:
     *     m = 1 - amount * (1 - env * level)
     * level 0 is silent, an OFF step is a gap whatever its level (env is 0
     * there, so the term vanishes), and the global amount is the dry/wet.
     *
     * `step_level` is the STRUCK step's, latched at gate-open -- not
     * depth[r->step], which is a different number the moment a release or a
     * tie outlives the step that started it. See its declaration. */
    float m = 1.0f - in->amount * (1.0f - in->env * in->step_level);

    in->step_pos += r->inc;
    r->frac += r->inc;
    while (r->frac >= 1.0) { r->frac -= 1.0; if (++r->step >= r->length) r->step = 0; }
    while (r->frac < 0.0)  { r->frac += 1.0; if (--r->step < 0) r->step = r->length - 1; }
    return m;
}

void tg_core_process_i16(tg_core_t *in, int16_t *lr, int frames, const tg_transport_t *t) {
    if (!in || !lr || frames <= 0) return;
    tg_run_t r;
    if (!tg_block_setup(in, frames, t, &r)) return;
    for (int i = 0; i < frames; i++) {
        float m = tg_next_gain(in, &r);
        float l = (float)lr[i * 2]     * m;
        float rr= (float)lr[i * 2 + 1] * m;
        lr[i * 2]     = (int16_t)(l  > 32767.0f ? 32767.0f : (l  < -32768.0f ? -32768.0f : l));
        lr[i * 2 + 1] = (int16_t)(rr > 32767.0f ? 32767.0f : (rr < -32768.0f ? -32768.0f : rr));
    }
}

void tg_core_process_f32(tg_core_t *in, float *lr, int frames, const tg_transport_t *t) {
    if (!in || !lr || frames <= 0) return;
    tg_run_t r;
    if (!tg_block_setup(in, frames, t, &r)) return;
    for (int i = 0; i < frames; i++) {
        float m = tg_next_gain(in, &r);
        lr[i * 2]     *= m;
        lr[i * 2 + 1] *= m;
    }
}

/* No clamping on the float paths, deliberately: the gate only ever
 * ATTENUATES (m is in 0..1), so it cannot push a signal out of range, and a
 * plugin host is entitled to headroom above 1.0 that we must not steal. */
void tg_core_process_f32_split(tg_core_t *in, float *l, float *rch, int frames,
                               const tg_transport_t *t) {
    if (!in || !l || !rch || frames <= 0) return;
    tg_run_t r;
    if (!tg_block_setup(in, frames, t, &r)) return;
    for (int i = 0; i < frames; i++) {
        float m = tg_next_gain(in, &r);
        l[i]   *= m;
        rch[i] *= m;
    }
}

/* --------------------------------------------------------------- params -- */

static void pattern_defaults(tg_pattern_t *p, int slot) {
    p->length = 16;
    tg_mask_zero(&p->ties);
    tg_mask_zero(&p->steps);
    for (int i = 0; i < TG_MAX_STEPS; i++) p->depth[i] = TG_DEPTH_FULL;
    /* Slot 1 is every other step -- the plainest thing that is audibly a gate
     * the moment the module is loaded. The rest start fully open, which is
     * silence-free rather than "the effect is broken". */
    /* Slot 1 is every other step; the rest start fully open. Written through
     * the bit helpers so the initial pattern cannot silently depend on the
     * mask being exactly one word wide. */
    for (int i = 0; i < 16; i++)
        tg_mask_set(&p->steps, i, (slot == 0) ? (i % 2 == 0) : 1);
}

/* One step's duration in ms, at the rate and tempo currently known. Called
 * whenever either changes, so the `ui` readout never reports a stale one. */
static void recalc_ms_per_step(tg_instance_t *in) {
    double bpm = (in->last_bpm > 1.0f) ? (double)in->last_bpm : 120.0;
    double sr  = (in->sample_rate > 0.0) ? in->sample_rate : 44100.0;
    double samples = (60.0 / bpm) * sr * tg_rates[in->rate_idx].beats;
    if (samples < 1.0) samples = 1.0;
    in->ms_per_step = (float)(samples * 1000.0 / sr);
}

tg_core_t *tg_core_create(double sample_rate) {     tg_instance_t *in = (tg_instance_t *)calloc(1, sizeof(tg_instance_t));
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
    in->sample_rate = (sample_rate > 0.0) ? sample_rate : 44100.0;
    /*
     * A STEP HAS A DURATION BEFORE IT HAS EVER BEEN PLAYED.
     *
     * ms_per_step is set on every block, so it was zero until the first one
     * ran -- and the `ui` readout is the only place a UI can learn how long a
     * step is. A host that opens the editor before processing (or between
     * projects) therefore got 0, which is indistinguishable from "no rate",
     * and the plugin's envelope panel drew nothing at all. Seeding it from
     * the default rate at a nominal 120 BPM removes the zero state: the
     * number is what the step WOULD last, and the first block replaces it
     * with the host's real tempo anyway.
     */
    in->last_bpm = 120.0f;
    recalc_ms_per_step(in);
    return in;
}

void tg_core_destroy(tg_core_t *c) { free(c); }

/*
 * HEX IS LSB-ALIGNED, AND THAT IS WHAT KEEPS OLD PATCHES READABLE.
 *
 * The mask used to be one uint32 printed with %X, so "5555" meant steps
 * 0,2,4,... Reading right-to-left into word 0 first gives a 128-bit mask the
 * same meaning, so a v3 blob -- which never has more than 8 hex digits --
 * lands exactly where it did. And emitting the minimal form (below) writes
 * "5555" again for any pattern inside 32 steps, so a short pattern's state is
 * byte-identical to what the previous version wrote. No format version bump,
 * no migration, and a patch moves between the two builds untouched.
 */
static void set_pattern_hex(tg_mask_t *dst, const char *val) {
    if (!val) return;
    tg_mask_zero(dst);
    /* Walk from the END of the string: the last character is the low nibble. */
    int len = (int)strlen(val);
    int bit = 0;
    for (int i = len - 1; i >= 0 && bit < TG_MAX_STEPS; i--) {
        int c = val[i], v;
        if      (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue;                       /* skip whitespace, 0x, junk */
        for (int k = 0; k < 4 && bit < TG_MAX_STEPS; k++, bit++)
            if ((v >> k) & 1) tg_mask_set(dst, bit, 1);
    }
}

/* The minimal hex for a mask: no leading zeros, and only as many words as
 * carry anything. Writes "0" for an empty mask rather than nothing. */
static int mask_to_hex(const tg_mask_t *m, char *out, int out_len) {
    int top = TG_MASK_WORDS - 1;
    while (top > 0 && m->w[top] == 0) top--;
    int n = snprintf(out, out_len, "%X", m->w[top]);
    for (int k = top - 1; k >= 0 && n > 0 && n < out_len; k--)
        n += snprintf(out + n, out_len - n, "%08X", m->w[k]);
    return n;
}

void tg_core_set_param(tg_core_t *instance, const char *key, const char *val) {
    tg_instance_t *in = (tg_instance_t *)instance;
    if (!in || !key || !val) return;
    tg_pattern_t *p = &in->pat[in->slot];

    if (strcmp(key, "slot") == 0) {
        int s = atoi(val);                   /* the wire is the OPTION INDEX */
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
        /* The `ui` readout carries the step DURATION, and it used to be
         * computed only inside a block -- so a rate changed while the host
         * was idle reported the old subdivision's length until audio ran
         * again. The plugin draws its envelope against that number, so the
         * picture simply disagreed with the knob. */
        recalc_ms_per_step(in);
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
            int mode;
            if (strcmp(val, "Off") == 0)      mode = 0;
            else if (strcmp(val, "On") == 0)  mode = 1;
            else if (strcmp(val, "Tie") == 0) mode = 2;
            else { mode = atoi(val); if (mode < 0) mode = 0; if (mode > 2) mode = 2; }

            tg_mask_set(&p->steps, c, mode != 0);
            tg_mask_set(&p->ties,  c, mode == 2);
        }
    } else if (strcmp(key, "step_amount") == 0) {
        int c = in->cursor;
        if (c >= 0 && c < TG_MAX_STEPS) {
            float f = clampf((float)atof(val), 0.0f, 1.0f);
            p->depth[c] = (uint8_t)(f * 255.0f + 0.5f);
        }
    } else if (strcmp(key, "legato") == 0) {
        in->legato = (strcmp(val, "On") == 0 || strcmp(val, "on") == 0 ||
                      atoi(val) != 0) ? 1 : 0;
    } else if (strcmp(key, "time_mode") == 0) {
        /* Names as well as the index: the Move shell wires this enum by index
         * while a patch or a plugin may well say what it means. */
        in->time_mode = (strcmp(val, "%") == 0 || strcmp(val, "Step") == 0 ||
                         strcmp(val, "step") == 0 || atoi(val) != 0)
                      ? TG_TIME_PCT : TG_TIME_MS;
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
        /* Absent in a pre-legato blob, and 0 is the behaviour those patches
         * had -- so an old patch loads sounding exactly as it did. */
        in->legato = 0;
        if (json_get_number(val, "legato", &n) == 0) in->legato = (n >= 0.5) ? 1 : 0;
        /* Absent in a pre-% blob, and MS is what those patches meant -- so an
         * old patch loads sounding exactly as it did, with no version bump
         * needed to say so. */
        in->time_mode = TG_TIME_MS;
        if (json_get_number(val, "tmode", &n) == 0)
            in->time_mode = (n >= 0.5) ? TG_TIME_PCT : TG_TIME_MS;
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
                /* The two masks are up to 32 hex digits now, so they are read
                 * as TEXT and handed to the LSB-aligned parser -- %x would cap
                 * them at whatever an unsigned holds and silently drop steps
                 * 32 and up. A v3 blob's 8-digit field parses identically. */
                char stx[40] = {0}, tix[40] = {0};
                int len = 16, used = 0;
                if (sscanf(sv, "%39[0-9a-fA-F]:%39[0-9a-fA-F]:%d%n",
                           stx, tix, &len, &used) == 3) {
                    set_pattern_hex(&in->pat[s].steps, stx);
                    set_pattern_hex(&in->pat[s].ties,  tix);
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

int tg_core_get_param(tg_core_t *instance, const char *key, char *buf, int buf_len) {
    tg_instance_t *in = (tg_instance_t *)instance;
    if (!in || !key) return -1;
    const tg_pattern_t *p = &in->pat[in->slot];

    if (strcmp(key, "name") == 0)    return snprintf(buf, buf_len, "TRANCE GATE");
    if (strcmp(key, "slot") == 0)    return snprintf(buf, buf_len, "%d", in->slot);
    if (strcmp(key, "length") == 0)  return snprintf(buf, buf_len, "%d", p->length - 1);
    if (strcmp(key, "rate") == 0)    return snprintf(buf, buf_len, "%s", tg_rates[in->rate_idx].label);
    if (strcmp(key, "attack") == 0)  return snprintf(buf, buf_len, "%.1f", in->attack_ms);
    if (strcmp(key, "decay") == 0)   return snprintf(buf, buf_len, "%.1f", in->decay_ms);
    if (strcmp(key, "sustain") == 0) return snprintf(buf, buf_len, "%.2f", in->sustain);
    if (strcmp(key, "release") == 0) return snprintf(buf, buf_len, "%.1f", in->release_ms);
    if (strcmp(key, "hold") == 0)    return snprintf(buf, buf_len, "%.2f", in->hold);
    if (strcmp(key, "amount") == 0)  return snprintf(buf, buf_len, "%.2f", in->amount);
    if (strcmp(key, "legato") == 0)  return snprintf(buf, buf_len, "%d", in->legato);
    if (strcmp(key, "time_mode") == 0) return snprintf(buf, buf_len, "%d", in->time_mode);
    /* The step's length in ms, so a shell can show what a % actually costs
     * without duplicating the rate table. */
    if (strcmp(key, "ms_per_step") == 0)
        return snprintf(buf, buf_len, "%.2f", in->ms_per_step);
    if (strcmp(key, "cursor") == 0) return snprintf(buf, buf_len, "%d", in->cursor);
    if (strcmp(key, "step") == 0) {
        const char *w = !tg_mask_get(&p->steps, in->cursor) ? "Off"
                      : (tg_mask_get(&p->ties, in->cursor) ? "Tie" : "On");
        return snprintf(buf, buf_len, "%s", w);
    }
    if (strcmp(key, "step_amount") == 0)
        return snprintf(buf, buf_len, "%.2f", p->depth[in->cursor] * (1.0f / 255.0f));
    if (strcmp(key, "pattern") == 0) return mask_to_hex(&p->steps, buf, buf_len);
    if (strcmp(key, "ties") == 0)    return mask_to_hex(&p->ties, buf, buf_len);

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
        /* Same minimal hex the state uses, so a UI that already parses one
         * parses the other -- and a <=32-step pattern still reports the exact
         * 8-digit string the previous version did. */
        char stx[40], tix[40];
        mask_to_hex(&p->steps, stx, sizeof(stx));
        mask_to_hex(&p->ties,  tix, sizeof(tix));
        int n = snprintf(buf, buf_len, "%s:%s:%d:%.3f:%.2f:%d:%d:",
                         stx, tix, length, pos,
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
            "\"hold\":%.3f,\"amount\":%.3f,\"legato\":%d,\"tmode\":%d",
            TG_STATE_VERSION, in->slot, tg_rates[in->rate_idx].label,
            in->attack_ms, in->decay_ms, in->sustain, in->release_ms,
            in->hold, in->amount, in->legato, in->time_mode);
        for (int s = 0; s < TG_SLOTS && n > 0 && n < buf_len; s++) {
            char stx[40], tix[40];
            mask_to_hex(&in->pat[s].steps, stx, sizeof(stx));
            mask_to_hex(&in->pat[s].ties,  tix, sizeof(tix));
            n += snprintf(buf + n, buf_len - n, ",\"p%d\":\"%s:%s:%d:",
                          s, stx, tix, in->pat[s].length);
            /*
             * TRAILING DEFAULTS ARE NOT WRITTEN.
             *
             * This used to emit TG_MAX_STEPS depths unconditionally -- 64
             * characters per slot at 32 steps, and 256 at 128, whether or not
             * a single one differed from full. The reader already treats an
             * absent depth as FULL (that is how v1 blobs load), so the last
             * non-default entry is the only place worth stopping at: a
             * pattern with no accents now writes no depths at all.
             *
             * It is what keeps a realistic 8-slot patch inside a bus
             * insert's 1024 bytes at the new length, and it changes nothing
             * about what a reader gets back.
             */
            int last = -1;
            for (int i = 0; i < TG_MAX_STEPS; i++)
                if (in->pat[s].depth[i] != TG_DEPTH_FULL) last = i;
            for (int i = 0; i <= last && n > 0 && n + 2 < buf_len; i++) {
                n += snprintf(buf + n, buf_len - n, "%02X", in->pat[s].depth[i]);
            }
            if (n > 0 && n < buf_len) n += snprintf(buf + n, buf_len - n, "\"");
        }
        if (n > 0 && n < buf_len) n += snprintf(buf + n, buf_len - n, "}");
        return (n >= buf_len) ? -1 : n;
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
void tg_core_on_midi(tg_core_t *in, const uint8_t *msg, int len) {
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


/* ------------------------------------------------------- sample rate -- */

void tg_core_set_sample_rate(tg_core_t *c, double sample_rate) {
    if (!c || sample_rate <= 0.0) return;
    c->sample_rate = sample_rate;
    /* ms_per_step cancels the sample rate out, so this changes nothing today.
     * It is here so that "ms_per_step is current" holds at every door into
     * the struct rather than at the two that happen to matter. */
    recalc_ms_per_step(c);
}

double tg_core_get_sample_rate(const tg_core_t *c) {
    return c ? c->sample_rate : 0.0;
}
