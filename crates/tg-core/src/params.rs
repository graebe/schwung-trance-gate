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

impl Instance {
    pub fn set_param(&mut self, key: &str, val: &str) {
        match key {
            "slot" => {
                let s = fmt::atoi(val); /* the wire is the OPTION INDEX */
                if s >= 0 && (s as usize) < crate::SLOTS {
                    self.slot = s as usize;
                    /* The cursor is GLOBAL and the length is PER SLOT, so
                     * switching to a shorter pattern can leave it past the
                     * end -- where every edit lands on a step the ring never
                     * draws. The length branch below clamps for the same
                     * reason; both doors need the same lock. */
                    let len = self.pat[self.slot].length;
                    if self.cursor >= len {
                        self.cursor = len - 1;
                    }
                }
            }
            "length" => {
                /* Option INDEX, as for `cursor`: index 15 is the option named
                 * "16", which is a length of 16. */
                let n = fmt::atoi(val) + 1;
                let slot = self.slot;
                self.pat[slot].length = n.clamp(1, MAX_STEPS as i64) as usize;
                if self.cursor >= self.pat[slot].length {
                    self.cursor = self.pat[slot].length - 1;
                }
            }
            "rate" => {
                self.rate_idx = rates::index_from(val);
                /* The `ui` readout carries the step DURATION, and it used to
                 * be computed only inside a block -- so a rate changed while
                 * the host was idle reported the old subdivision's length
                 * until audio ran again. The plugin draws its envelope
                 * against that number, so the picture disagreed with the
                 * knob. */
                self.recalc_ms_per_step();
            }
            "attack" => self.attack = clampf(fmt::atof(val) as f32, 0.0, STAGE_MAX_PCT),
            "decay" => self.decay = clampf(fmt::atof(val) as f32, 0.0, STAGE_MAX_PCT),
            "sustain" => self.sustain = clampf(fmt::atof(val) as f32, 0.0, 1.0),
            "hold" => self.hold = clampf(fmt::atof(val) as f32, 0.0, 1.0),
            "release" => self.release = clampf(fmt::atof(val) as f32, 0.0, STAGE_MAX_PCT),
            "amount" => self.amount = clampf(fmt::atof(val) as f32, 0.0, 1.0),
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
                self.legato = val == "On" || val == "on" || fmt::atoi(val) != 0;
            }
            "curve" => {
                let c = match val {
                    "Exponential" | "Exp" => 1,
                    "S-Curve" | "S" => 2,
                    _ => fmt::atoi(val) as i32,
                };
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
            "time_mode" => {
                /* Names as well as the index: the Move shell wires this enum
                 * by index while a patch or a plugin may well say what it
                 * means. */
                self.time_mode = if val == "%" || val == "Step" || val == "step"
                    || fmt::atoi(val) != 0
                {
                    TimeMode::Pct
                } else {
                    TimeMode::Ms
                };
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
