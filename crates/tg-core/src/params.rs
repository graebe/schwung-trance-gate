/*!
`set_param` and `get_param` -- the string-keyed surface both shells drive.

Every value crosses a `char*`, so this module is where C's conventions live:
lenient parsing ([`fmt::atof`]), `snprintf`-shaped output, and the wire
conventions each key speaks. Two of those are load-bearing and easy to get
backwards, so they are stated where they are used rather than here.
*/

use crate::envelope::{Curve, Stage};
use crate::fmt::{self, Buf};
use crate::mask::Mask;
use crate::{rates, Instance, TimeMode, DEPTH_FULL, MAX_STEPS, STAGE_MAX_PCT};
use core::fmt::Write;

#[inline]
fn clampf(x: f32, lo: f32, hi: f32) -> f32 {
    if x < lo { lo } else if x > hi { hi } else { x }
}

/*
 * HEX IS LSB-ALIGNED, AND THAT IS WHAT KEEPS OLD PATCHES READABLE.
 *
 * The mask used to be one u32 printed with %X, so "5555" meant steps
 * 0,2,4,... Reading right-to-left into word 0 first gives a 128-bit mask the
 * same meaning, so a v3 blob -- which never has more than 8 hex digits --
 * lands exactly where it did. And emitting the minimal form writes "5555"
 * again for any pattern inside 32 steps, so a short pattern's state is
 * byte-identical to what the previous version wrote.
 */
pub fn set_pattern_hex(dst: &mut Mask, val: &str) {
    dst.clear();
    /* Walk from the END of the string: the last character is the low nibble. */
    let b = val.as_bytes();
    let mut bit = 0usize;
    for &c in b.iter().rev() {
        if bit >= MAX_STEPS {
            break;
        }
        let v = match c {
            b'0'..=b'9' => (c - b'0') as u32,
            b'a'..=b'f' => (c - b'a') as u32 + 10,
            b'A'..=b'F' => (c - b'A') as u32 + 10,
            _ => continue, /* skip whitespace, 0x, junk */
        };
        for k in 0..4 {
            if bit >= MAX_STEPS {
                break;
            }
            if (v >> k) & 1 != 0 {
                dst.set(bit, true);
            }
            bit += 1;
        }
    }
}

/*
 * THE AUTOMATABLE PARAMETERS, BY NUMBER.
 *
 * [`Instance::set_param`] is the canonical door and takes strings, which is
 * right for a patch, a pattern or a pad edit -- all of them message-thread
 * work. It is wrong for HOST AUTOMATION, which arrives on the audio thread: a
 * float formatted and parsed back costs a locale-dependent conversion in each
 * direction (C's `atof` honours `LC_NUMERIC`, so a comma-decimal host turns
 * "0.750" into 0) and a string-match ladder, per value, per block.
 *
 * These are the same twelve values on the same wire conventions -- slot,
 * length and rate are INDICES, legato and time_mode are 0|1, the rest are the
 * units the string keys use -- with the decimal detour removed. `set_param`
 * is implemented in terms of [`Instance::set_num`], so every clamp exists
 * once.
 *
 * THE DISCRIMINANTS ARE THE C ABI. `tg_param_t` is this enum's order, and a
 * host that saved an automation lane saved these numbers, so inserting one in
 * the middle silently rewires a user's project.
 */
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Param {
    Slot = 0,
    Length,
    Rate,
    Legato,
    TimeMode,
    Curve,
    Amount,
    Hold,
    Attack,
    Decay,
    Sustain,
    Release,
}

impl Param {
    /// The C side passes an `int`. Anything outside the enum is dropped
    /// rather than clamped onto a neighbour: a wrong parameter silently
    /// moving a different control is worse than one doing nothing.
    pub fn from_i32(v: i32) -> Option<Param> {
        use Param::*;
        Some(match v {
            0 => Slot,
            1 => Length,
            2 => Rate,
            3 => Legato,
            4 => TimeMode,
            5 => Curve,
            6 => Amount,
            7 => Hold,
            8 => Attack,
            9 => Decay,
            10 => Sustain,
            11 => Release,
            _ => return None,
        })
    }
}

impl Instance {
    /// The twelve automatable values, by number. Every clamp and every side
    /// effect lives here; [`Instance::set_param`] parses a string and
    /// delegates, so the two doors cannot drift apart.
    ///
    /// Audio-thread safe: a match, a clamp and a store. No allocation, no
    /// formatting, no locale.
    pub fn set_num(&mut self, param: Param, value: f64) {
        /*
         * `as i32` SATURATES IN RUST WHERE C'S CAST IS UNDEFINED. For an
         * out-of-range double C commonly lands on INT_MIN, which every branch
         * below treats as out of range and rejects -- and saturation lands on
         * i32::MAX or i32::MIN, which they reject identically. Same outcome,
         * one of them defined.
         */
        match param {
            Param::Slot => {
                let s = value as i32; /* the wire is the OPTION INDEX */
                if s >= 0 && (s as usize) < crate::SLOTS {
                    self.slot = s as usize;
                    /* The cursor is GLOBAL and the length is PER SLOT, so
                     * switching to a shorter pattern can leave it past the
                     * end -- where every edit lands on a step the ring never
                     * draws. */
                    let len = self.pat[self.slot].length;
                    if self.cursor >= len {
                        self.cursor = len - 1;
                    }
                }
            }
            Param::Length => {
                /* Option INDEX, as for `cursor`: index 15 is the option named
                 * "16", which is a length of 16. */
                let n = value as i64 + 1;
                let slot = self.slot;
                self.pat[slot].length = n.clamp(1, MAX_STEPS as i64) as usize;
                if self.cursor >= self.pat[slot].length {
                    self.cursor = self.pat[slot].length - 1;
                }
            }
            Param::Rate => {
                /* OUT OF RANGE IS THE DEFAULT, NOT THE NEAREST END --
                 * `rates::index_from` has answered that way since indices
                 * were first accepted, and an old state blob may carry one.
                 * Clamping here instead would have been a second convention
                 * for the same wire, differing only in the case nobody looks
                 * at. A host parameter is a 13-way choice and never sends
                 * anything else, so this is about the patch door, not the
                 * knob. */
                let i = value as i32;
                self.rate_idx = if i >= 0 && (i as usize) < rates::RATES.len() {
                    i as usize
                } else {
                    rates::RATE_DEFAULT
                };
                /* The `ui` readout carries the step DURATION, and it used to
                 * be computed only inside a block -- so a rate changed while
                 * the host was idle reported the old subdivision's length
                 * until audio ran again. The plugin draws its envelope
                 * against that number. */
                self.recalc_ms_per_step();
            }
            Param::Legato => self.legato = value != 0.0,
            Param::TimeMode => {
                self.time_mode = if value != 0.0 { TimeMode::Pct } else { TimeMode::Ms }
            }
            Param::Curve => {
                let c = value as i32;
                let c = Curve::from_i32(if (0..=2).contains(&c) { c } else { 0 });

                /*
                 * RE-ANCHOR, OR THE CHANGE IS A CLICK.
                 *
                 * `env.t` is a position along the STAGE, and the shape
                 * decides what level that position means. Halfway through a
                 * stage is 0.50 linear and 0.82 exponential, so swapping the
                 * shape under a live gate moves the gain instantly -- exactly
                 * the fault the attack ramp and the level latch were both
                 * added to avoid.
                 *
                 * Solving shape_new(t') = shape_old(t) keeps the LEVEL and
                 * changes only the trajectory from here.
                 */
                if c != self.curve {
                    if self.env.t > 0.0 && self.env.t < 1.0 {
                        let w = crate::envelope::shape(self.curve, self.env.t);
                        self.env.t = crate::envelope::shape_inv(c, w);
                    }
                    self.curve = c;
                }
            }
            Param::Amount => self.amount = clampf(value as f32, 0.0, 1.0),
            Param::Hold => self.hold = clampf(value as f32, 0.0, 1.0),
            Param::Sustain => self.sustain = clampf(value as f32, 0.0, 1.0),
            Param::Attack => self.attack = clampf(value as f32, 0.0, STAGE_MAX_PCT),
            Param::Decay => self.decay = clampf(value as f32, 0.0, STAGE_MAX_PCT),
            Param::Release => self.release = clampf(value as f32, 0.0, STAGE_MAX_PCT),
        }
    }

    pub fn set_param(&mut self, key: &str, val: &str) {
        match key {
            /* The twelve automatable keys parse and delegate -- `set_num`
             * owns every clamp and every side effect, so the numeric and
             * string doors cannot drift. */
            "slot" => self.set_num(Param::Slot, fmt::atoi(val) as f64),
            "length" => self.set_num(Param::Length, fmt::atoi(val) as f64),
            /* A LABEL first, a bare number as an index -- `rates::index_from`
             * owns that convention. */
            "rate" => self.set_num(Param::Rate, rates::index_from(val) as f64),
            "attack" => self.set_num(Param::Attack, fmt::atof(val)),
            "decay" => self.set_num(Param::Decay, fmt::atof(val)),
            "sustain" => self.set_num(Param::Sustain, fmt::atof(val)),
            "hold" => self.set_num(Param::Hold, fmt::atof(val)),
            "release" => self.set_num(Param::Release, fmt::atof(val)),
            "amount" => self.set_num(Param::Amount, fmt::atof(val)),
            "cursor" => {
                /*
                 * THE WIRE CARRIES THE OPTION INDEX, and the option NAMES
                 * carry the step numbers -- so index 15 displays as "16".
                 *
                 * It used to send the 1-based name with `options_as_string`,
                 * the other legal convention, and it displayed one too high:
                 * the host has three resolvers for an enum's wire format and
                 * only two consult that flag. Indices are the host's default
                 * convention, so speaking them makes all three agree.
                 */
                let c = fmt::atoi(val).max(0) as usize;
                let len = self.pat[self.slot].length;
                self.cursor = if c >= len { len - 1 } else { c };
            }
            "step" => {
                /*
                 * THE ONE KNOB THAT EDITS THE PATTERN, three-state rather
                 * than two because a tie is not a separate property of a step
                 * -- it is the third thing a step can be. Off / On / Tie maps
                 * onto the two bits, costs one knob instead of two, and
                 * cannot express the meaningless fourth combination (tied
                 * while off).
                 */
                let c = self.cursor;
                if c < MAX_STEPS {
                    let mode = match val {
                        "Off" => 0,
                        "On" => 1,
                        "Tie" => 2,
                        _ => fmt::atoi(val).clamp(0, 2),
                    };
                    let slot = self.slot;
                    self.pat[slot].steps.set(c, mode != 0);
                    self.pat[slot].ties.set(c, mode == 2);
                }
            }
            "step_amount" => {
                let c = self.cursor;
                if c < MAX_STEPS {
                    let f = clampf(fmt::atof(val) as f32, 0.0, 1.0);
                    let slot = self.slot;
                    self.pat[slot].depth[c] = (f * 255.0 + 0.5) as u8;
                }
            }
            "legato" => {
                let on = val == "On" || val == "on" || fmt::atoi(val) != 0;
                self.set_num(Param::Legato, on as i32 as f64);
            }
            "curve" => {
                /* Names as well as the index; the re-anchor that keeps a
                 * mid-gate change from clicking lives in `set_num` with the
                 * rest. */
                let c = match val {
                    "Exponential" | "Exp" => 1,
                    "S-Curve" | "S" => 2,
                    _ => fmt::atoi(val) as i32,
                };
                self.set_num(Param::Curve, c as f64);
            }
            "time_mode" => {
                /* Names as well as the index: the Move shell wires this enum
                 * by index while a patch or a plugin may well say what it
                 * means. */
                let pct =
                    val == "%" || val == "Step" || val == "step" || fmt::atoi(val) != 0;
                self.set_num(Param::TimeMode, pct as i32 as f64);
            }
            "pattern" => {
                let slot = self.slot;
                set_pattern_hex(&mut self.pat[slot].steps, val);
            }
            "ties" => {
                let slot = self.slot;
                set_pattern_hex(&mut self.pat[slot].ties, val);
            }
            "state" => crate::state::load(self, val),
            _ => {}
        }
    }

    /// Returns the length written, or -1 for a key this engine does not serve
    /// -- which is how a shell knows to answer its own.
    pub fn get_param(&self, key: &str, out: &mut [u8]) -> i32 {
        let mut b = Buf::new(out);
        let p = self.pattern();
        let _ = match key {
            "name" => write!(b, "TRANCE GATE"),
            "slot" => write!(b, "{}", self.slot),
            "length" => write!(b, "{}", p.length - 1),
            "rate" => write!(b, "{}", rates::RATES[self.rate_idx].label),
            "attack" => fmt::f(&mut b, self.attack as f64, 1),
            "decay" => fmt::f(&mut b, self.decay as f64, 1),
            "sustain" => fmt::f(&mut b, self.sustain as f64, 2),
            "release" => fmt::f(&mut b, self.release as f64, 1),
            "hold" => fmt::f(&mut b, self.hold as f64, 2),
            "amount" => fmt::f(&mut b, self.amount as f64, 2),
            "legato" => write!(b, "{}", self.legato as i32),
            "time_mode" => write!(b, "{}", self.time_mode as i32),
            "curve" => write!(b, "{}", self.curve as i32),
            /* The step's length in ms, so a shell can show what a % actually
             * costs without duplicating the rate table. */
            "ms_per_step" => fmt::f(&mut b, self.ms_per_step as f64, 2),
            /* How long the gate is open for. A shell showing a stage in
             * MILLISECONDS needs this and the percentage:
             * ms = value/100 * width_ms. Served rather than left to the shell
             * to recompute, so the rate table and the hold clamp stay in one
             * place. */
            "width_ms" => fmt::f(&mut b, self.width_ms() as f32 as f64, 2),
            "cursor" => write!(b, "{}", self.cursor),
            "step" => {
                let w = if !p.steps.get(self.cursor) {
                    "Off"
                } else if p.ties.get(self.cursor) {
                    "Tie"
                } else {
                    "On"
                };
                write!(b, "{}", w)
            }
            "step_amount" => fmt::f(&mut b, (p.depth[self.cursor] as f32 * (1.0 / 255.0)) as f64, 2),
            "pattern" => p.steps.to_hex(&mut b),
            "ties" => p.ties.to_hex(&mut b),
            /* The live playhead. Declared "live", so the host re-reads
             * `phase:effective` every tick instead of once per value rotation
             * -- the difference between an animated ring and a slideshow. */
            "phase" | "phase:effective" => {
                let length = p.length.max(1) as f64;
                let mut pos = self.step_pos % length;
                if pos < 0.0 {
                    pos += length;
                }
                fmt::f(&mut b, pos, 3)
            }
            /*
             * ONE READ FOR THE TWELVE AUTOMATABLE VALUES.
             *
             * `ui` carries the pattern and the playhead; it carries no part
             * of the SOUND, which is why a shell that wants to know whether
             * its picture is stale has to ask for nine keys one at a time --
             * nine locks and nine buffers, thirty times a second, to answer
             * "did anything move".
             *
             * This is that set in one line: exactly the values with a host
             * parameter behind them, in the order the plugin declares them,
             * plus `width_ms` because a shell showing a stage in milliseconds
             * needs it to convert and would otherwise take a tenth lock to
             * get it.
             *
             *   slot:legato:time_mode:curve:rate:length:amount:hold:attack:
             *   decay:sustain:release:width_ms
             *
             * FLOATS ARE %.9g, WHICH IS NOT COSMETIC. Nine significant digits
             * is FLT_DECIMAL_DIG -- the shortest precision for which
             * float -> decimal -> float is the identity. The single-key
             * getters round to %.1f and %.2f, so a shell that reads a value
             * and writes it back quantises the patch every time it does so.
             * Anything that round-trips through this readout must come back
             * bit-identical, or the caller needs a suppression flag and every
             * suppression flag eventually drops something real.
             *
             * `length` is the OPTION INDEX and `rate` is the LABEL, both
             * exactly as the single-key getters answer them -- one convention
             * per key, not two. attack/decay/release are PERCENTAGES OF
             * WIDTH, like everywhere else.
             */
            "params" => {
                let r = write!(
                    b,
                    "{}:{}:{}:{}:{}:{}:",
                    self.slot,
                    self.legato as i32,
                    self.time_mode as i32,
                    self.curve as i32,
                    rates::RATES[self.rate_idx].label,
                    p.length - 1
                );
                r.and_then(|_| {
                    for v in [
                        self.amount,
                        self.hold,
                        self.attack,
                        self.decay,
                        self.sustain,
                        self.release,
                    ] {
                        fmt::g(&mut b, v as f64, 9)?;
                        b.write_char(':')?;
                    }
                    fmt::g(&mut b, self.width_ms(), 9)
                })
            }
            "ui" => return self.ui_readout(b),
            "state" => return crate::state::save(self, b),
            _ => return -1,
        };
        b.finish()
    }

    /*
     * ONE READ FOR THE WHOLE PICTURE.
     *
     * The animated page needs six facts and a param read is ~2.8 ms, so
     * asking for them separately would cost six rotation stops -- and an
     * `extra_keys` value is refreshed on the SLOW rotation only, so six keys
     * would be six times slower than one, not merely six reads.
     *
     *   steps : ties : length : phase : ms_step : advancing : cursor : depths
     */
    fn ui_readout(&self, mut b: Buf) -> i32 {
        let p = self.pattern();
        let length = p.length.max(1);
        let mut pos = self.step_pos % length as f64;
        if pos < 0.0 {
            pos += length as f64;
        }
        let _ = p.steps.to_hex(&mut b);
        let _ = write!(b, ":");
        let _ = p.ties.to_hex(&mut b);
        let _ = write!(b, ":{}:", length);
        let _ = fmt::f(&mut b, pos, 3);
        let _ = write!(b, ":");
        let _ = fmt::f(&mut b, self.ms_per_step as f64, 2);
        let _ = write!(b, ":{}:{}:", self.advancing as i32, self.cursor);
        /* Per-step depths as a run of two hex digits each -- one field rather
         * than many, because the page already pays for this string once per
         * rotation stop and a second read would halve the anchor rate. */
        for i in 0..length {
            let _ = write!(b, "{:02X}", p.depth[i]);
        }
        b.finish()
    }

    /// The envelope's stage, for a shell that wants to know whether a gate is
    /// live without inferring it from the level.
    pub fn stage(&self) -> Stage {
        self.env.stage
    }

    pub fn reset_depths(&mut self, slot: usize) {
        self.pat[slot].depth = [DEPTH_FULL; MAX_STEPS];
    }
}
