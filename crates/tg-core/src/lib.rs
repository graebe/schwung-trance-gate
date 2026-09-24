/*!
Trance Gate -- a tempo-locked step gate with a per-step ADSR.

Modelled on the Kilohearts Trance Gate: 8 pattern slots, a pattern of up to
128 steps with ties, a Rate (the length of one step), an ADSR applied at each
step, and an Amount.

# Threading

Every entry point runs on an audio callback. On Move that is SCHED_FIFO 70,
pinned to core 3, with ~2370us of slack per 128-frame block; in a plugin it is
whatever the host provides. There is no control thread.

So: **no allocation outside [`Instance::new`], no I/O, no locks, no logging**.
The workspace sets `panic = "abort"` because unwinding out of `extern "C"`
into a C host is undefined behaviour, and a crash the OS reports is better
than one that corrupts the host's stack on the way out.
*/

pub mod envelope;
pub mod fmt;
pub mod mask;
pub mod params;
pub mod rates;
pub mod state;

use envelope::{Curve, Env, Stage, StageLens};
use mask::Mask;

pub const MAX_STEPS: usize = 128;
pub const SLOTS: usize = 8;
pub const DEPTH_FULL: u8 = 255;

/// A stage runs to twice the gate's width and no further -- past that it
/// cannot finish under any Width, so the extra range would be knob travel
/// with nothing on the end of it.
pub const STAGE_MAX_PCT: f32 = 200.0;

/// What the envelope's three time values MEAN to a shell showing them. The
/// engine does not consult it: ms and % are two readings of one number.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum TimeMode {
    Ms = 0,
    Pct = 1,
}

/// Beyond this much error, jump rather than glide.
const RESYNC_STEPS: f64 = 0.25;
/// Fraction of the phase error absorbed per block.
const TRACK_GAIN: f64 = 0.05;

/// What the host says about the transport.
#[derive(Clone, Copy, Default)]
pub struct Transport {
    pub running: bool,
    pub beats: f64,
    pub bpm: f32,
}

#[derive(Clone)]
pub struct Pattern {
    pub steps: Mask,
    pub ties: Mask,
    /// 1..=MAX_STEPS
    pub length: usize,
    /// Per-step level, 0..255. An ACCENT: the global Amount scales the whole
    /// sequence on top of it, so this says "how much of the gate" and Amount
    /// says "how much gating". 255 is the neutral value, which is why a v1
    /// blob without the array must fill it rather than zero it.
    pub depth: [u8; MAX_STEPS],
}

impl Pattern {
    fn new(slot: usize) -> Self {
        let mut p = Self {
            steps: Mask::new(),
            ties: Mask::new(),
            length: 16,
            depth: [DEPTH_FULL; MAX_STEPS],
        };
        /* Slot 1 is every other step -- the plainest thing that is audibly a
         * gate the moment the module is loaded. The rest start fully open,
         * which is silence-free rather than "the effect is broken". Written
         * through the bit helpers so the initial pattern cannot silently
         * depend on the mask being exactly one word wide. */
        for i in 0..16 {
            p.steps.set(i, if slot == 0 { i % 2 == 0 } else { true });
        }
        p
    }

    #[inline]
    pub fn on(&self, i: usize) -> bool {
        self.steps.get(i)
    }
    #[inline]
    pub fn tied(&self, i: usize) -> bool {
        self.ties.get(i)
    }
}

pub struct Instance {
    pub pat: Vec<Pattern>,
    pub slot: usize,
    pub rate_idx: usize,

    /*
     * ATTACK, DECAY AND RELEASE ARE PERCENTAGES OF THE GATE'S WIDTH, 0..200.
     *
     * Not milliseconds, despite the wire keys, which are kept because they
     * appear in every saved patch. 100% is "exactly fills the gate"; 200% is
     * "twice the gate", a stage that never finishes before the gate shuts.
     *
     * Measured against WIDTH and not against the step because the step is not
     * the musical unit here -- the gate's open time is. It also makes ms and
     * % two readings of ONE number: ms is `value/100 * width_ms`, so its
     * maximum moves with the rate and with Width while the percentage stays
     * put. See [`Instance::stage_samples`].
     */
    pub attack: f32,
    pub decay: f32,
    /// 0..1 -- a LEVEL, not a duration.
    pub sustain: f32,
    pub release: f32,

    /*
     * How much of a step the gate stays open, 0..1.
     *
     * SUSTAIN IS A LEVEL AND HAS NO LENGTH -- in an ADSR it holds until the
     * note ends, and here "the note" is the step. That is correct and it is
     * also not what someone reaching for a shorter gate wants. This is the
     * control they are reaching for: release begins this far into the step
     * rather than at its end, which is a sequencer's gate length.
     */
    pub hold: f32,
    /// How much the gate acts, 0..1. 1 == a closed gate is silent, 0 == the
    /// effect is bypassed.
    pub amount: f32,
    /// Edit position on the ring, 0..length-1.
    pub cursor: usize,

    // ---- runtime, not saved ----
    pub step_pos: f64,
    /// Step index at the previous sample; `None` = none.
    pub last_step: Option<usize>,
    pub was_running: bool,
    pub env: Env,
    /*
     * THE STRUCK STEP'S LEVEL, HELD FOR THE WHOLE GATE.
     *
     * Read fresh every sample as `depth[current_step]`, this is wrong in the
     * two places where a gate and a step are not the same span: a RELEASE
     * outliving its step was scaled by the NEXT step's amount, and a TIE
     * stepped the level mid-gate -- a discontinuity in the gain, which is a
     * click. Latched when the envelope enters ATTACK and held until IDLE.
     */
    pub step_level: f32,

    /// Published for the UI, computed once per block, because `get_param`
    /// runs on the audio callback too and must stay trivial.
    pub ms_per_step: f32,
    pub last_bpm: f32,
    /// "The playhead is moving" -- the UI's extrapolator is the only reader
    /// and is written against the concept, not against what drives it.
    pub advancing: bool,
    /// Adjacent ON steps hold as ONE gate instead of re-articulating.
    pub legato: bool,
    pub time_mode: TimeMode,
    pub curve: Curve,
    pub sample_rate: f64,
}

impl Instance {
    pub fn new(sample_rate: f64) -> Self {
        let mut me = Self {
            pat: (0..SLOTS).map(Pattern::new).collect(),
            slot: 0,
            rate_idx: rates::RATE_DEFAULT,
            /* The percentages that reproduce the old 2 / 20 / 20 ms defaults
             * against a full-width 1/16 step at 120 BPM, so a fresh instance
             * sounds as it always did. */
            attack: 1.6,
            decay: 16.0,
            sustain: 1.0,
            release: 16.0,
            hold: 1.0,
            amount: 1.0,
            cursor: 0,
            step_pos: 0.0,
            last_step: None,
            was_running: false,
            env: Env::default(),
            step_level: 0.0,
            ms_per_step: 0.0,
            last_bpm: 120.0,
            advancing: false,
            legato: false,
            time_mode: TimeMode::Ms,
            curve: Curve::Linear,
            sample_rate: if sample_rate > 0.0 { sample_rate } else { 44100.0 },
        };
        me.recalc_ms_per_step();
        me
    }

    #[inline]
    pub fn pattern(&self) -> &Pattern {
        &self.pat[self.slot]
    }

    /// One step's duration in ms, at the rate and tempo currently known.
    /// Called whenever either changes, so the `ui` readout never reports a
    /// stale one -- a value that only appeared after the first block meant a
    /// blank panel on open.
    pub fn recalc_ms_per_step(&mut self) {
        let bpm = if self.last_bpm > 1.0 { self.last_bpm as f64 } else { 120.0 };
        let sr = if self.sample_rate > 0.0 { self.sample_rate } else { 44100.0 };
        let mut samples = (60.0 / bpm) * sr * rates::RATES[self.rate_idx].beats;
        if samples < 1.0 {
            samples = 1.0;
        }
        self.ms_per_step = (samples * 1000.0 / sr) as f32;
    }

    /// How long the gate is open for, in ms: Width of a step. The unit every
    /// envelope stage is measured in.
    #[inline]
    pub fn width_ms(&self) -> f64 {
        self.hold as f64 * self.ms_per_step as f64
    }

    /// How long a stage lasts, in samples. `value` is a percentage of the
    /// gate's WIDTH, 0..200.
    #[inline]
    fn stage_samples(&self, value: f32) -> f64 {
        value as f64 * 0.01 * self.width_ms() * (self.sample_rate / 1000.0)
    }

    #[inline]
    fn lens(&self) -> StageLens {
        StageLens {
            attack: self.stage_samples(self.attack),
            decay: self.stage_samples(self.decay),
            release: self.stage_samples(self.release),
            sustain: self.sustain,
        }
    }

    /// The playhead's position in the pattern, 0..1. Cheap and
    /// allocation-free -- for a caller on the audio thread, where the
    /// equivalent formatted readout's `snprintf` does not belong.
    pub fn phase01(&self) -> f64 {
        let length = self.pattern().length.max(1) as f64;
        let mut pos = self.step_pos % length;
        if pos < 0.0 {
            pos += length;
        }
        pos / length
    }

    /*
     * A step boundary. This is the whole of what a tie means: an ON step
     * arriving on top of a held ON step does NOT restart the envelope.
     */
    fn on_step_boundary(&mut self, prev_step: Option<usize>, new_step: usize) {
        let l = self.lens();
        let p = &self.pat[self.slot];
        let on_now = p.on(new_step);
        let on_prev = prev_step.map_or(false, |s| p.on(s));
        let tied = prev_step.map_or(false, |s| p.tied(s));

        if on_now {
            /*
             * LEGATO IS "TREAT EVERY ADJACENT PAIR AS TIED".
             *
             * At Sustain 100% this changes nothing audible, because attack
             * already ramps from wherever the envelope is and there is
             * nowhere to ramp from a fully open gate. The difference appears
             * BELOW 100%, which is where a retrigger actually re-articulates.
             */
            if !(on_prev && (tied || self.legato)) {
                let lvl = p.depth[new_step] as f32 * (1.0 / 255.0);

                /*
                 * THE ATTACK STARTS WHERE THE GAIN IS, NOT WHERE `env` IS.
                 *
                 * `env` is only half the gain: what you hear is `env * level`,
                 * and the level changes at this very boundary. `att_from`
                 * carries `env` across, so after a half-filled pad the
                 * envelope resumed at the right ENV and instantly the wrong
                 * GAIN -- the click the att_from ramp was added to prevent,
                 * arriving through the other factor.
                 *
                 * `env` may land ABOVE 1 -- a loud pad followed by a quiet
                 * one -- and that is not a special case: the attack is a lerp
                 * from att_from to 1, so it ramps DOWN to the new level over
                 * the attack time instead of up.
                 */
                let gain_now = self.env.level as f64 * self.step_level as f64;
                self.step_level = lvl;
                self.env.level = if lvl > 1.0e-6 {
                    (gain_now / lvl as f64) as f32
                } else {
                    0.0
                };
                self.env.enter(Stage::Attack, &l);
            }
        } else if on_prev || self.env.stage != Stage::Idle {
            self.env.enter(Stage::Release, &l);
        }
    }

    /// The gain for one sample, advancing every bit of state that depends on
    /// it.
    #[inline]
    fn next_gain(&mut self, r: &mut Run) -> f32 {
        if Some(r.step) != self.last_step {
            let prev = self.last_step;
            self.on_step_boundary(prev, r.step);
            self.last_step = Some(r.step);
        }

        /* Gate length: release inside the step, not only at its edge. A tie
         * means "hold through", so shortening it would contradict the tie. */
        if self.hold < 1.0
            && self.env.stage != Stage::Release
            && self.env.stage != Stage::Idle
        {
            let p = &self.pat[self.slot];
            if r.frac >= self.hold as f64 && !(p.on(r.step) && p.tied(r.step)) {
                let l = self.lens();
                self.env.enter(Stage::Release, &l);
            }
        }

        let l = self.lens();
        self.env.advance(self.curve, &l);

        /* The step's amount is how far the gate OPENS, not how far it closes:
         *     m = 1 - amount * (1 - env * level)
         * level 0 is silent, an OFF step is a gap whatever its level (env is
         * 0 there, so the term vanishes), and the global amount is the
         * dry/wet.
         *
         * `step_level` is the STRUCK step's, latched at gate-open -- not
         * `depth[r.step]`, which is a different number the moment a release
         * or a tie outlives the step that started it. */
        let m = 1.0 - self.amount * (1.0 - self.env.level * self.step_level);

        self.step_pos += r.inc;
        r.frac += r.inc;
        while r.frac >= 1.0 {
            r.frac -= 1.0;
            r.step += 1;
            if r.step >= r.length {
                r.step = 0;
            }
        }
        while r.frac < 0.0 {
            r.frac += 1.0;
            if r.step == 0 {
                r.step = r.length - 1;
            } else {
                r.step -= 1;
            }
        }
        m
    }

    /*
     * PHASE IS ANCHORED PER BLOCK AND ADVANCED PER SAMPLE.
     *
     * The host's beat position is interpolated per block and its intra-tick
     * fraction is CLAMPED, so a late tick freezes phase instead of
     * overshooting. Differencing it per sample therefore renders that plateau
     * as an audible stutter on every late clock tick. Instead the host's
     * answer is an anchor that a local accumulator is pulled gently towards
     * -- a phase-locked loop, not a clock divider.
     *
     * Returns `None` when the block needs no gain applied at all, having
     * already advanced whatever state the next block depends on. The caller
     * leaves the buffer untouched, which is exactly right: the dry signal is
     * already in it.
     */
    fn block_setup(&mut self, frames: usize, t: Option<&Transport>) -> Option<Run> {
        let length = self.pattern().length.clamp(1, MAX_STEPS);

        let mut bpm = 120.0f32;
        if let Some(t) = t {
            if t.bpm > 1.0 && t.bpm < 1000.0 {
                bpm = t.bpm;
            }
        }

        let beats_per_step = rates::RATES[self.rate_idx].beats;
        let mut samples_per_step = (60.0 / bpm as f64) * self.sample_rate * beats_per_step;
        if samples_per_step < 1.0 {
            samples_per_step = 1.0;
        }
        let mut inc = 1.0 / samples_per_step;

        /* A stopped transport is not beat 0, it is no beat at all. */
        let beats = match t {
            Some(t) if t.running => t.beats,
            _ => -1.0,
        };
        let running = beats >= 0.0;

        self.last_bpm = bpm;
        self.ms_per_step = (samples_per_step * 1000.0 / self.sample_rate) as f32;
        self.advancing = running;

        if running {
            let target = beats / beats_per_step;
            if !self.was_running {
                /* Transport just started: land exactly, do not glide in. */
                self.step_pos = target;
                self.last_step = None;
            } else {
                let err = target - self.step_pos;
                if err > RESYNC_STEPS || err < -RESYNC_STEPS {
                    self.step_pos = target; /* loop, seek or tempo jump */
                    /* A jump re-evaluates the step even when it lands on the
                     * same index: the boundary test is `step != last_step`,
                     * so a seek back onto the step we were already on would
                     * fire nothing and leave the envelope where it was. */
                    self.last_step = None;
                } else {
                    inc += (err * TRACK_GAIN) / frames as f64;
                }
            }
        } else {
            /* Stopped means open: hold the gate open and park at step 0, so
             * the next start is a downbeat rather than wherever the pattern
             * stopped. */
            self.step_pos = 0.0;
            self.last_step = None;
            self.env.stage = Stage::Idle;
            self.env.level = 0.0;
            self.was_running = false;
            return None;
        }
        self.was_running = running;

        /* Amount zero is a true bypass -- m collapses to exactly 1.0 -- so do
         * not spend a block proving it. The phase still advances, so turning
         * it back up lands on the step the pattern would have reached. */
        if self.amount <= 0.0 {
            self.step_pos += inc * frames as f64;
            return None;
        }

        let frac = self.step_pos - self.step_pos.floor();
        let mut step = (self.step_pos.floor() % length as f64) as i64;
        if step < 0 {
            step += length as i64;
        }
        Some(Run {
            length,
            inc,
            frac,
            step: step as usize,
        })
    }

    /*
     * ONE SET OF MATHS, THREE BUFFER FORMATS.
     *
     * Move hands us int16 interleaved; VST3 and AU hand us float, usually as
     * separate channel pointers. Writing the loop three times would mean
     * three places for the gain law to drift, and the drift would be
     * inaudible until somebody A/B'd the plugin against the hardware.
     */
    pub fn process_i16(&mut self, lr: &mut [i16], frames: usize, t: Option<&Transport>) {
        let Some(mut r) = self.block_setup(frames, t) else { return };
        for i in 0..frames {
            let m = self.next_gain(&mut r);
            let l = lr[i * 2] as f32 * m;
            let rr = lr[i * 2 + 1] as f32 * m;
            lr[i * 2] = l.clamp(-32768.0, 32767.0) as i16;
            lr[i * 2 + 1] = rr.clamp(-32768.0, 32767.0) as i16;
        }
    }

    pub fn process_f32(&mut self, lr: &mut [f32], frames: usize, t: Option<&Transport>) {
        let Some(mut r) = self.block_setup(frames, t) else { return };
        for i in 0..frames {
            let m = self.next_gain(&mut r);
            lr[i * 2] *= m;
            lr[i * 2 + 1] *= m;
        }
    }

    /// No clamping on the float paths, deliberately: the gate only ever
    /// ATTENUATES (m is in 0..1), so it cannot push a signal out of range,
    /// and a plugin host is entitled to headroom above 1.0 we must not steal.
    pub fn process_f32_split(
        &mut self,
        l: &mut [f32],
        rch: &mut [f32],
        frames: usize,
        t: Option<&Transport>,
    ) {
        let Some(mut r) = self.block_setup(frames, t) else { return };
        for i in 0..frames {
            let m = self.next_gain(&mut r);
            l[i] *= m;
            rch[i] *= m;
        }
    }
}

/// Per-block state the sample loop walks.
struct Run {
    length: usize,
    inc: f64,
    frac: f64,
    step: usize,
}
