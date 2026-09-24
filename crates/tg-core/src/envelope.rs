/*!
The envelope: its shapes, and the stage machine.

THE SHAPE IS A WARP ON TIME. Every stage has the form `f(env_t)` with `env_t`
running 0..1 across it, so a curve is not three new formulas -- it is one
function substituted for `env_t` in the three that exist:

```text
ATTACK   env = att_from + (1 - att_from) * w
DECAY    env = 1        - (1 - sustain)  * w
RELEASE  env = rel_from * (1 - w)
```

Every shape obeys `shape(0) = 0`, `shape(1) = 1` and is monotonic, so a stage
still starts and ends exactly where it did and still takes the time it was
given. Only the path between changes.

LINEAR RETURNS `t` UNTOUCHED, which is what keeps the reference render
bit-identical. It is also why the three expressions are not tidied into a
shared `lerp`: `rel_from * (1 - w)` and `rel_from - rel_from * w` are one
number in algebra and two in floating point.
*/

/// The path a stage takes between its endpoints.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(i32)]
pub enum Curve {
    Linear = 0,
    Exp = 1,
    SCurve = 2,
}

impl Curve {
    pub fn from_i32(v: i32) -> Self {
        match v {
            1 => Curve::Exp,
            2 => Curve::SCurve,
            _ => Curve::Linear,
        }
    }
}

/// The bend. Chosen so the curve is clearly audible without being a step:
/// halfway through an exponential stage the envelope is ~82% of the way.
const CURVE_K: f64 = 3.0;
/// `1 - exp(-3)`, spelled out exactly as the C does so the division is the
/// same division.
const DENOM: f64 = 0.95021293163213605;

/// Fast, then easing into the target -- what "exponential envelope" means on
/// hardware, and the direction every stage takes because the three
/// expressions above already point it the right way for each.
#[inline]
fn curve_exp(t: f64) -> f64 {
    (1.0 - (-CURVE_K * t).exp()) / DENOM
}

#[inline]
fn curve_exp_inv(w: f64) -> f64 {
    let x = 1.0 - w * DENOM;
    if x <= 1e-12 {
        return 1.0;
    }
    -x.ln() / CURVE_K
}

#[inline]
pub fn shape(curve: Curve, t: f64) -> f64 {
    if t <= 0.0 {
        return 0.0;
    }
    if t >= 1.0 {
        return 1.0;
    }
    match curve {
        Curve::Exp => curve_exp(t),
        /* TWO EXPONENTIALS, JOINED. The first half is the exponential
         * mirrored (slow, then accelerating), the second is it the right way
         * up -- so the pair is slow-fast-slow and meets in the middle at the
         * same slope, Einv'(1) being E'(0). A corner there would be a kink in
         * the gain, which is audible as surely as a step. */
        Curve::SCurve => {
            if t < 0.5 {
                0.5 * (1.0 - curve_exp(1.0 - 2.0 * t))
            } else {
                0.5 + 0.5 * curve_exp(2.0 * t - 1.0)
            }
        }
        Curve::Linear => t,
    }
}

/// The inverse, which is what lets the curve change mid-gate without a click:
/// see the re-anchor in `set_param`. Monotonic and analytic for all three.
#[inline]
pub fn shape_inv(curve: Curve, w: f64) -> f64 {
    if w <= 0.0 {
        return 0.0;
    }
    if w >= 1.0 {
        return 1.0;
    }
    match curve {
        Curve::Exp => curve_exp_inv(w),
        Curve::SCurve => {
            if w < 0.5 {
                0.5 * (1.0 - curve_exp_inv(1.0 - 2.0 * w))
            } else {
                0.5 + 0.5 * curve_exp_inv(2.0 * w - 1.0)
            }
        }
        Curve::Linear => w,
    }
}

/// Which part of the envelope is running.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Stage {
    Idle,
    Attack,
    Decay,
    Sustain,
    Release,
}

/// The envelope's whole mutable state, split out of the instance so the stage
/// machine can be read without the pattern data around it.
#[derive(Clone, Copy, Debug)]
pub struct Env {
    pub stage: Stage,
    /// Position through the current stage as 0..1, advanced by a PRECOMPUTED
    /// reciprocal. It used to hold a sample COUNT and divide by the stage
    /// length every sample -- a double division per sample for a quotient
    /// whose denominator cannot change inside a stage.
    pub t: f64,
    /// `1 / stage length in samples`; 0 for a zero-length stage.
    pub inc: f64,
    pub level: f32,
    /// Env level when RELEASE began.
    pub rel_from: f32,
    /// Env level when ATTACK began.
    ///
    /// ATTACK NEEDS THE SAME MEMORY RELEASE ALWAYS HAD, and not having it was
    /// the click: an attack restarted from SILENCE wherever the envelope
    /// actually was, so at the boundary between two adjacent ON steps the
    /// gain went 1.000 -> 0.000 in ONE SAMPLE.
    pub att_from: f32,
}

impl Default for Env {
    fn default() -> Self {
        Self {
            stage: Stage::Idle,
            t: 0.0,
            inc: 0.0,
            level: 0.0,
            rel_from: 0.0,
            att_from: 0.0,
        }
    }
}

/// The three stage lengths, in samples, as the caller has already worked them
/// out. Passed in rather than reached for so the stage machine does not need
/// the whole instance.
#[derive(Clone, Copy)]
pub struct StageLens {
    pub attack: f64,
    pub decay: f64,
    pub release: f64,
    pub sustain: f32,
}

impl Env {
    /*
     * Entering a stage, with zero-length stages walked THROUGH rather than
     * recursed through.
     *
     * This was env_enter calling env_settle calling env_enter: bounded in
     * fact (attack -> decay -> sustain is the longest chain a zero can open)
     * but the bound lived in two functions and a guard counter, so it read as
     * unbounded. A 0 ms attack must not spend a sample reporting env == 0 --
     * that is an audible click at the step edge -- which is the whole reason
     * the walk exists.
     */
    pub fn enter(&mut self, mut stage: Stage, l: &StageLens) {
        loop {
            self.stage = stage;
            self.t = 0.0;

            let len = match stage {
                Stage::Attack => {
                    self.att_from = self.level;
                    l.attack
                }
                Stage::Decay => l.decay,
                Stage::Release => {
                    self.rel_from = self.level;
                    l.release
                }
                _ => 0.0,
            };

            /* The one division a stage pays, taken once instead of per
             * sample. Guarded by the zero-length walk below, so it is only
             * ever taken on a positive length. */
            self.inc = if len > 0.0 { 1.0 / len } else { 0.0 };

            /* SUSTAIN and IDLE have no length and are where the walk stops. */
            if !matches!(stage, Stage::Attack | Stage::Decay | Stage::Release) {
                return;
            }
            if len > 0.0 {
                return;
            }

            stage = match stage {
                Stage::Attack => {
                    self.level = 1.0;
                    Stage::Decay
                }
                Stage::Decay => {
                    self.level = l.sustain;
                    Stage::Sustain
                }
                _ => {
                    self.level = 0.0;
                    Stage::Idle
                }
            };
        }
    }

    #[inline]
    pub fn advance(&mut self, curve: Curve, l: &StageLens) {
        /* The shape, evaluated once and substituted for env_t below. Linear
         * hands back env_t itself, so those three lines stay the arithmetic
         * they were. */
        let w = shape(curve, self.t);

        match self.stage {
            Stage::Attack => {
                /* FROM WHERE IT IS, not from zero -- the same thing RELEASE
                 * does with rel_from. It still REACHES 1.0 and still takes
                 * attack_ms to get there; it simply does not fall off a cliff
                 * first. */
                self.level = (self.att_from as f64 + (1.0 - self.att_from as f64) * w) as f32;
                self.t += self.inc;
                if self.t >= 1.0 {
                    self.level = 1.0;
                    self.enter(Stage::Decay, l);
                }
            }
            Stage::Decay => {
                self.level = (1.0 - (1.0 - l.sustain as f64) * w) as f32;
                self.t += self.inc;
                if self.t >= 1.0 {
                    self.level = l.sustain;
                    self.enter(Stage::Sustain, l);
                }
            }
            Stage::Sustain => self.level = l.sustain,
            Stage::Release => {
                self.level = (self.rel_from as f64 * (1.0 - w)) as f32;
                self.t += self.inc;
                if self.t >= 1.0 {
                    self.level = 0.0;
                    self.enter(Stage::Idle, l);
                }
            }
            Stage::Idle => self.level = 0.0,
        }
    }
}
