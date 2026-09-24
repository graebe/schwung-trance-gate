/*!
The `state` blob: one string that carries a whole patch.

# Versions, and why there is a number at all

A version field costs nothing and is the only thing that can tell one reading
of a number from another. The Ducker shipped without one, and a 0.1.x blob's
envelope times -- read as the units the next version introduced -- collapsed
the effect silently on a patch that had been working.

- **2**: patterns gained a per-step DEPTH array. A v1 blob has none, and the
  absent value must load as FULL; loading it as zero would silently mute every
  gate in every patch that already works.
- **3**: `depth` folded into `mix`. They were one quantity with two names --
  only their PRODUCT ever reached the audio -- so a v2 blob's migration
  multiplies them, and a saved patch sounds identical rather than jumping to
  full wet.
- **4**: attack/decay/release are PERCENTAGES OF WIDTH, not milliseconds. The
  keys did not change, so only the version can tell a v3's `"attack": 2.0`
  (2 ms) from a v4's (2% of the gate).
*/

use crate::envelope::Curve;
use crate::fmt::{self, Buf};
use crate::params::set_pattern_hex;
use crate::{rates, Instance, TimeMode, DEPTH_FULL, MAX_STEPS, SLOTS, STAGE_MAX_PCT};
use core::fmt::Write;

pub const STATE_VERSION: i32 = 4;

#[inline]
fn clampf(x: f32, lo: f32, hi: f32) -> f32 {
    if x < lo { lo } else if x > hi { hi } else { x }
}

/* Tiny JSON scanners. No allocation, no locale surprises -- the same two the
 * C had, for the same reason: the blob is ours, it is flat, and a parser
 * would be more code than the format. */
fn find_value<'a>(json: &'a str, key: &str) -> Option<&'a str> {
    let mut needle = [0u8; 64];
    let mut n = 0;
    for &b in b"\"" {
        needle[n] = b;
        n += 1;
    }
    for &b in key.as_bytes() {
        if n + 2 >= needle.len() {
            return None;
        }
        needle[n] = b;
        n += 1;
    }
    needle[n] = b'"';
    n += 1;
    needle[n] = b':';
    n += 1;
    let needle = core::str::from_utf8(&needle[..n]).ok()?;
    let at = json.find(needle)? + needle.len();
    Some(json[at..].trim_start_matches([' ', '\t']))
}

fn get_number(json: &str, key: &str) -> Option<f64> {
    find_value(json, key).map(fmt::atof)
}

fn get_string<'a>(json: &'a str, key: &str) -> Option<&'a str> {
    let v = find_value(json, key)?;
    let rest = v.strip_prefix('"')?;
    let end = rest.find('"')?;
    Some(&rest[..end])
}

pub fn load(inst: &mut Instance, val: &str) {
    /* Which format this blob is in; 0 when absent, which the oldest
     * pre-version blobs are. */
    let sv_num = get_number(val, "sv").unwrap_or(0.0) as i32;

    if let Some(n) = get_number(val, "slot") {
        if n >= 0.0 && (n as usize) < SLOTS {
            inst.slot = n as usize;
        }
    }
    if let Some(s) = get_string(val, "rate") {
        inst.rate_idx = rates::index_from(s);
    } else if let Some(n) = get_number(val, "rate") {
        /* A numeric rate is an INDEX and must be resolved as one. Passing ""
         * here instead silently reset every such blob to the default -- a
         * patch that loads, reports a rate, and runs at another. */
        let n = n as i64;
        if n >= 0 && (n as usize) < rates::RATES.len() {
            inst.rate_idx = n as usize;
        }
    }

    /* Read raw and clamp LATER: a v3 blob's numbers are milliseconds and do
     * not fit the 0..200 a percentage does, so clamping here would flatten
     * every legacy attack above 200 ms before it could be converted. */
    let raw_att = get_number(val, "attack");
    let raw_dec = get_number(val, "decay");
    let raw_rel = get_number(val, "release");

    if let Some(n) = get_number(val, "sustain") {
        inst.sustain = clampf(n as f32, 0.0, 1.0);
    }
    /* Absent in v1 and v2 blobs, where the gate always ran the whole step;
     * 1.0 is that behaviour, so an old patch is unchanged. */
    inst.hold = 1.0;
    /* Absent in a pre-legato blob, and off is what those patches did. */
    inst.legato = get_number(val, "legato").map_or(false, |n| n >= 0.5);
    /* Absent in a pre-% blob, and MS is what those patches meant. */
    inst.time_mode = match get_number(val, "tmode") {
        Some(n) if n >= 0.5 => TimeMode::Pct,
        _ => TimeMode::Ms,
    };
    /* Absent in a pre-curve blob, and straight lines are what those patches
     * sounded like. */
    inst.curve = match get_number(val, "curve") {
        Some(n) => Curve::from_i32((n + 0.5) as i32),
        None => Curve::Linear,
    };
    if let Some(n) = get_number(val, "hold") {
        inst.hold = clampf(n as f32, 0.0, 1.0);
    }

    /*
     * THE STAGES, ONCE RATE AND WIDTH ARE BOTH KNOWN.
     *
     * A v3 blob holds absolute milliseconds and a v4 one percentages of the
     * gate's width, so a legacy patch is CONVERTED rather than reinterpreted
     * -- 2 ms read as 2% would be a patch that loads and sounds like a
     * different patch.
     *
     * The conversion needs the width, which needs the rate AND hold, and hold
     * is parsed just above. Hence the raw reads earlier and the arithmetic
     * here rather than in place.
     *
     * IT ASSUMES 120 BPM, because a patch does not carry the tempo it was
     * written at. `ms_per_step` is seeded at that tempo and only a running
     * transport replaces it, so a patch written at 120 converts exactly and
     * one written elsewhere converts proportionally -- the best available
     * without a tempo to read, and why the version exists rather than a
     * silent reinterpretation.
     */
    inst.recalc_ms_per_step();
    let legacy_ms = sv_num > 0 && sv_num < 4;
    let w = inst.width_ms();
    let to_pct = if legacy_ms && w > 1.0e-6 { 100.0 / w } else { 1.0 };
    if let Some(v) = raw_att {
        inst.attack = clampf((v * to_pct) as f32, 0.0, STAGE_MAX_PCT);
    }
    if let Some(v) = raw_dec {
        inst.decay = clampf((v * to_pct) as f32, 0.0, STAGE_MAX_PCT);
    }
    if let Some(v) = raw_rel {
        inst.release = clampf((v * to_pct) as f32, 0.0, STAGE_MAX_PCT);
    }

    /*
     * THREE SPELLINGS OF ONE VALUE, AND THE OLD PAIR MULTIPLIES.
     *
     * v3 writes `amount`. v2 wrote `mix` and `depth`, which spanned one
     * degree of freedom between them, so the faithful migration is that
     * product. Reading just one would make every patch saved with depth < 1
     * jump to full -- louder gating on a patch that had been working.
     */
    if let Some(n) = get_number(val, "amount") {
        inst.amount = clampf(n as f32, 0.0, 1.0);
    } else {
        let mix = get_number(val, "mix");
        let depth = get_number(val, "depth");
        if mix.is_some() || depth.is_some() {
            let m = clampf(mix.unwrap_or(1.0) as f32, 0.0, 1.0);
            let d = clampf(depth.unwrap_or(1.0) as f32, 0.0, 1.0);
            inst.amount = clampf(m * d, 0.0, 1.0);
        }
    }

    /* Patterns travel as one field per slot so a slot cannot be restored
     * half-applied. */
    for s in 0..SLOTS {
        let mut key = [0u8; 8];
        key[0] = b'p';
        let d = s as u8 + b'0';
        key[1] = d;
        let key = core::str::from_utf8(&key[..2]).unwrap_or("p0");
        let Some(field) = get_string(val, key) else { continue };

        /* "<steps>:<ties>:<length>[:<depths>]". The two masks are up to 32
         * hex digits now, so they are read as TEXT and handed to the
         * LSB-aligned parser -- a %x would cap them at whatever an unsigned
         * holds and silently drop steps 32 and up. A v3 blob's 8-digit field
         * parses identically. */
        let mut parts = field.splitn(4, ':');
        let (Some(stx), Some(tix), Some(lens)) = (parts.next(), parts.next(), parts.next())
        else {
            continue;
        };
        set_pattern_hex(&mut inst.pat[s].steps, stx);
        set_pattern_hex(&mut inst.pat[s].ties, tix);
        inst.pat[s].length = (fmt::atoi(lens)).clamp(1, MAX_STEPS as i64) as usize;

        /*
         * A V1 TRIPLE HAS NO DEPTHS, AND ABSENT MEANS FULL.
         *
         * Every patch saved before that version ends after the length.
         * Leaving the array at whatever it held -- or zeroing it -- would
         * load those patches SILENT, with the pattern and the ring both
         * looking completely correct. This is the whole reason the state
         * version exists.
         */
        inst.pat[s].depth = [DEPTH_FULL; MAX_STEPS];
        if let Some(d) = parts.next() {
            let b = d.as_bytes();
            for i in 0..MAX_STEPS {
                let (Some(&h), Some(&l)) = (b.get(i * 2), b.get(i * 2 + 1)) else { break };
                let (Some(h), Some(l)) = (hexval(h), hexval(l)) else { break };
                inst.pat[s].depth[i] = h * 16 + l;
            }
        }
    }
}

fn hexval(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

pub fn save(inst: &Instance, mut b: Buf) -> i32 {
    let _ = write!(
        b,
        "{{\"sv\":{},\"slot\":{},\"rate\":\"{}\",",
        STATE_VERSION,
        inst.slot,
        rates::RATES[inst.rate_idx].label
    );
    let _ = write!(b, "\"attack\":");
    let _ = fmt::f(&mut b, inst.attack as f64, 2);
    let _ = write!(b, ",\"decay\":");
    let _ = fmt::f(&mut b, inst.decay as f64, 2);
    let _ = write!(b, ",\"sustain\":");
    let _ = fmt::f(&mut b, inst.sustain as f64, 3);
    let _ = write!(b, ",\"release\":");
    let _ = fmt::f(&mut b, inst.release as f64, 2);
    let _ = write!(b, ",\"hold\":");
    let _ = fmt::f(&mut b, inst.hold as f64, 3);
    let _ = write!(b, ",\"amount\":");
    let _ = fmt::f(&mut b, inst.amount as f64, 3);
    let _ = write!(
        b,
        ",\"legato\":{},\"tmode\":{},\"curve\":{}",
        inst.legato as i32, inst.time_mode as i32, inst.curve as i32
    );

    for s in 0..SLOTS {
        let _ = write!(b, ",\"p{}\":\"", s);
        let _ = inst.pat[s].steps.to_hex(&mut b);
        let _ = write!(b, ":");
        let _ = inst.pat[s].ties.to_hex(&mut b);
        let _ = write!(b, ":{}:", inst.pat[s].length);

        /*
         * TRAILING DEFAULTS ARE NOT WRITTEN.
         *
         * This used to emit MAX_STEPS depths unconditionally -- 256
         * characters per slot at 128 steps, whether or not a single one
         * differed from full. The reader already treats an absent depth as
         * FULL (that is how v1 blobs load), so the last non-default entry is
         * the only place worth stopping at: a pattern with no accents writes
         * no depths at all.
         *
         * It is what keeps a realistic 8-slot patch inside a bus insert's
         * 1024 bytes at the new length, and it changes nothing about what a
         * reader gets back.
         */
        let last = (0..MAX_STEPS)
            .rev()
            .find(|&i| inst.pat[s].depth[i] != DEPTH_FULL);
        if let Some(last) = last {
            for i in 0..=last {
                let _ = write!(b, "{:02X}", inst.pat[s].depth[i]);
            }
        }
        let _ = write!(b, "\"");
    }
    let _ = write!(b, "}}");
    b.finish()
}
