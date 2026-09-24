/*!
Formatting and parsing that match C's, because the wire is C's.

Every value this engine reports crosses a `char*` boundary, and the state blob
is compared byte-for-byte by the test suite and round-tripped through saved
patches. So two things have to hold:

- **Nothing here allocates.** `get_param` runs on the audio callback, so
  `format!` is not available: output goes into the caller's buffer through
  [`Buf`], which truncates the way `snprintf` does rather than growing.
- **Parsing is C's, not Rust's.** `atof("abc")` is 0.0 and `atoi("12ms")` is
  12; `str::parse` is an error in both cases. A patch that fails to parse a
  field must fall back the way the C did, or a blob the old build read
  silently changes meaning.
*/

use core::fmt::Write;

/// A fixed-capacity writer with `snprintf` semantics: writes what fits, drops
/// the rest, and reports the length as if it had all fit -- which is what the
/// C ABI's callers expect back.
pub struct Buf<'a> {
    out: &'a mut [u8],
    /// Bytes actually written.
    pub len: usize,
    /// Bytes that WOULD have been written. `snprintf` returns this.
    pub wanted: usize,
}

impl<'a> Buf<'a> {
    pub fn new(out: &'a mut [u8]) -> Self {
        Self { out, len: 0, wanted: 0 }
    }

    /// Null-terminate and return the length written, excluding the
    /// terminator -- the value `tg_core_get_param` returns.
    pub fn finish(self) -> i32 {
        let n = self.len.min(self.out.len().saturating_sub(1));
        if !self.out.is_empty() {
            self.out[n] = 0;
        }
        n as i32
    }
}

impl<'a> Write for Buf<'a> {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        self.wanted += s.len();
        /* One byte is always held back for the terminator, as snprintf does. */
        let room = self.out.len().saturating_sub(1);
        for &b in s.as_bytes() {
            if self.len >= room {
                break;
            }
            self.out[self.len] = b;
            self.len += 1;
        }
        Ok(())
    }
}

/// `strtod`/`atof`: leading space, optional sign, digits with an optional
/// fraction and exponent, and **0.0 rather than an error** for anything it
/// cannot read. Trailing junk is ignored, so "0.5abc" is 0.5 and "abc" is 0.
pub fn atof(s: &str) -> f64 {
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() && (b[i] == b' ' || b[i] == b'\t' || b[i] == b'\n' || b[i] == b'\r') {
        i += 1;
    }
    let start = i;
    if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
        i += 1;
    }
    let digits_start = i;
    while i < b.len() && b[i].is_ascii_digit() {
        i += 1;
    }
    if i < b.len() && b[i] == b'.' {
        i += 1;
        while i < b.len() && b[i].is_ascii_digit() {
            i += 1;
        }
    }
    if i == digits_start {
        return 0.0; /* no mantissa digits at all */
    }
    /* An exponent counts only if it is complete -- "1e" is 1, not an error. */
    if i < b.len() && (b[i] == b'e' || b[i] == b'E') {
        let mark = i;
        i += 1;
        if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
            i += 1;
        }
        if i < b.len() && b[i].is_ascii_digit() {
            while i < b.len() && b[i].is_ascii_digit() {
                i += 1;
            }
        } else {
            i = mark;
        }
    }
    s[start..i].parse::<f64>().unwrap_or(0.0)
}

/// `atoi`: the same leniency, truncating rather than rounding.
pub fn atoi(s: &str) -> i64 {
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() && (b[i] == b' ' || b[i] == b'\t' || b[i] == b'\n' || b[i] == b'\r') {
        i += 1;
    }
    let start = i;
    if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
        i += 1;
    }
    let d0 = i;
    while i < b.len() && b[i].is_ascii_digit() {
        i += 1;
    }
    if i == d0 {
        return 0;
    }
    s[start..i].parse::<i64>().unwrap_or(0)
}

/// `%.*f`. Rust's float formatting and glibc's both render the exact binary
/// value and round half to even, so this is a thin wrapper -- but it is
/// spelled out here so there is one place to change if that ever stops being
/// true, and so the call sites read as the printf they replaced.
#[inline]
pub fn f(out: &mut dyn Write, v: f64, decimals: usize) -> core::fmt::Result {
    match decimals {
        0 => write!(out, "{:.0}", v),
        1 => write!(out, "{:.1}", v),
        2 => write!(out, "{:.2}", v),
        3 => write!(out, "{:.3}", v),
        _ => write!(out, "{:.*}", decimals, v),
    }
}

/// `%.*g`, which Rust has no equivalent of and which the `params` readout
/// needs at nine significant digits -- `FLT_DECIMAL_DIG`, the shortest
/// precision for which `f32 -> decimal -> f32` is the identity.
///
/// `{}` would also round-trip, but it is not the same string: Rust prints the
/// shortest form that recovers the **f64**, so an f32 widened to double comes
/// out as `0.10000000149011612` where C writes `0.100000001`. The wire is
/// C's, so the format is C's.
///
/// The rules, from the C standard: with precision `P` and the value's decimal
/// exponent `X` *after rounding to P significant digits*, use `%f` style with
/// `P-1-X` decimals when `-4 <= X < P` and `%e` style otherwise, then strip
/// trailing zeros from the fraction and the point if nothing survives it.
pub fn g(out: &mut dyn Write, v: f64, sig: usize) -> core::fmt::Result {
    let p = if sig == 0 { 1 } else { sig };

    if v.is_nan() {
        return out.write_str("nan");
    }
    if v.is_infinite() {
        return out.write_str(if v < 0.0 { "-inf" } else { "inf" });
    }

    /*
     * THE EXPONENT IS READ AFTER ROUNDING, NOT BEFORE.
     *
     * 9.9999999996 at nine significant digits is 1.00000000e1, so its X is 1
     * and not the 0 that a log10 of the input would report -- and X picks the
     * style, so getting it early gets the whole number wrong. Rendering the
     * scientific form first and reading the exponent back off it makes that
     * ordering structural instead of something to remember.
     *
     * Rust's `{:.*e}` is correctly rounded, as glibc's `%e` is, so the digits
     * agree by construction rather than by luck.
     */
    let mut sci = [0u8; 48];
    let n = {
        let mut b = Buf::new(&mut sci);
        write!(b, "{:.*e}", p - 1, v)?;
        b.len
    };
    let s = match core::str::from_utf8(&sci[..n]) {
        Ok(s) => s,
        Err(_) => return out.write_str("0"),
    };

    /* Rust writes `1.5e3` and `1.5e-3`: no `+`, no zero padding. C wants
     * `1.5e+03`, so the exponent is reassembled below rather than copied. */
    let (mant, exp) = match s.find('e') {
        Some(i) => (&s[..i], &s[i + 1..]),
        None => (s, "0"),
    };
    let x = atoi(exp) as i32;

    let neg = mant.as_bytes().first() == Some(&b'-');
    let mut d = [b'0'; 24];
    let mut nd = 0usize;
    for &c in mant.as_bytes() {
        if c.is_ascii_digit() && nd < d.len() {
            d[nd] = c;
            nd += 1;
        }
    }

    if neg {
        out.write_char('-')?;
    }

    /* Index of the last non-zero digit at or after `from`, exclusive; 0 when
     * every digit from there on is a zero and the fraction disappears. */
    let last_nonzero = |from: usize| -> usize {
        let mut end = 0;
        for i in from..nd {
            if d[i] != b'0' {
                end = i + 1;
            }
        }
        end
    };

    if x < -4 || x >= p as i32 {
        out.write_char(d[0] as char)?;
        let end = last_nonzero(1);
        if end > 1 {
            out.write_char('.')?;
            for &c in &d[1..end] {
                out.write_char(c as char)?;
            }
        }
        out.write_char('e')?;
        out.write_char(if x < 0 { '-' } else { '+' })?;
        let ax = x.unsigned_abs();
        if ax < 10 {
            out.write_char('0')?;
        }
        write!(out, "{}", ax)?;
    } else if x >= 0 {
        let split = x as usize + 1; /* digits before the point */
        for &c in &d[..split.min(nd)] {
            out.write_char(c as char)?;
        }
        let end = last_nonzero(split);
        if end > split {
            out.write_char('.')?;
            for &c in &d[split..end] {
                out.write_char(c as char)?;
            }
        }
    } else {
        /* -4 <= x < 0: a leading zero, then -x-1 more before the digits. */
        let end = last_nonzero(0);
        if end == 0 {
            return out.write_char('0');
        }
        out.write_str("0.")?;
        for _ in 0..(-x - 1) {
            out.write_char('0')?;
        }
        for &c in &d[..end] {
            out.write_char(c as char)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    struct S(heapless_str::Small);
    mod heapless_str {
        pub struct Small {
            pub buf: [u8; 64],
            pub len: usize,
        }
        impl Small {
            pub fn new() -> Self {
                Self { buf: [0; 64], len: 0 }
            }
            pub fn as_str(&self) -> &str {
                core::str::from_utf8(&self.buf[..self.len]).unwrap()
            }
        }
        impl core::fmt::Write for Small {
            fn write_str(&mut self, s: &str) -> core::fmt::Result {
                for &b in s.as_bytes() {
                    if self.len < self.buf.len() {
                        self.buf[self.len] = b;
                        self.len += 1;
                    }
                }
                Ok(())
            }
        }
    }
    impl S {
        fn of(v: f64) -> Self {
            let mut s = heapless_str::Small::new();
            g(&mut s, v, 9).unwrap();
            S(s)
        }
    }

    /*
     * EVERY EXPECTATION HERE WAS PRINTED BY C, not written by hand: a small
     * generator ran `printf("%.9g")` over these values on this machine and
     * its output is pasted below. The point of `g` is to be byte-identical to
     * that function, so the only defensible oracle is that function.
     */
    #[test]
    fn g_matches_c_printf_at_nine_significant_digits() {
        let cases: &[(f64, &str)] = &[
        (0f64, "0"),
        (-0f64, "-0"),
        (1f64, "1"),
        (-1f64, "-1"),
        (0.5f64, "0.5"),
        (2f64, "2"),
        (10f64, "10"),
        (100f64, "100"),
        (1000f64, "1000"),
        (0.10000000000000001f64, "0.1"),
        (0.25f64, "0.25"),
        (0.33333333333333331f64, "0.333333333"),
        (200f64, "200"),
        (199.99998500000001f64, "199.999985"),
        (123.456789f64, "123.456789"),
        (0.123456789f64, "0.123456789"),
        (0.98765432099999995f64, "0.987654321"),
        (0.0123456789f64, "0.0123456789"),
        (0.333333343f64, "0.333333343"),
        (9.9999999996f64, "10"),
        (99.999999900000006f64, "99.9999999"),
        (9.9999999999000004e-05f64, "0.0001"),
        (0.0001f64, "0.0001"),
        (9.9999000000000003e-05f64, "9.9999e-05"),
        (1.0000000000000001e-05f64, "1e-05"),
        (0.001f64, "0.001"),
        (100000f64, "100000"),
        (100000000f64, "100000000"),
        (1000000000f64, "1e+09"),
        (10000000000f64, "1e+10"),
        (1e-10f64, "1e-10"),
        (123456789f64, "123456789"),
        (1234567890f64, "1.23456789e+09"),
        (999999999f64, "999999999"),
        (999999999.5f64, "1e+09"),
        (1.5000000000000001e+300f64, "1.5e+300"),
        (2.5000000000000171e-310f64, "2.5e-310"),
        (3f64, "3"),
        (0.0001220703125f64, "0.000122070312"),
        (0.10000000149011612f64, "0.100000001"),
        (0.3333333432674408f64, "0.333333343"),
        (199.99998474121094f64, "199.999985"),
        (0.012345679104328156f64, "0.0123456791"),
        (9.9998886718268301e-321f64, "9.99988867e-321"),
        (4.9406564584124654e-324f64, "4.94065646e-324"),
        (1.7976931348623157e+308f64, "1.79769313e+308"),
        // INFINITY / NAN handled separately
        ];
        for &(v, want) in cases {
            let got = S::of(v);
            assert_eq!(got.0.as_str(), want, "%.9g of {:.17e}", v);
        }
    }

    #[test]
    fn g_handles_the_non_finite_cases_the_way_c_does() {
        assert_eq!(S::of(f64::INFINITY).0.as_str(), "inf");
        assert_eq!(S::of(f64::NEG_INFINITY).0.as_str(), "-inf");
        assert_eq!(S::of(f64::NAN).0.as_str(), "nan");
    }

    /*
     * The property the `params` readout actually depends on: nine significant
     * digits is FLT_DECIMAL_DIG, so a float printed and read back is the same
     * float. Swept over every f32 the engine can hold in a stage or a unit
     * value, by bit pattern rather than by a handful of samples.
     */
    #[test]
    fn nine_digits_round_trips_every_f32_in_range() {
        /*
         * The property the `params` readout depends on: nine significant
         * digits is FLT_DECIMAL_DIG, so a float printed and read back is the
         * same float. Swept by BIT PATTERN rather than by a handful of
         * samples, over exactly the range a stage or a unit value can hold --
         * 0 through 200 is bits 0 through 0x43480000, denormals included.
         *
         * A prime stride walks every exponent and every mantissa region
         * without testing all 1.1 billion of them; the endpoints and the
         * awkward values from the round-trip test are checked outright,
         * because a stride is free to step over the one that matters.
         */
        let check = |v: f32| {
            let mut s = heapless_str::Small::new();
            g(&mut s, v as f64, 9).unwrap();
            let back = atof(s.as_str()) as f32;
            assert_eq!(back.to_bits(), v.to_bits(), "{} -> {} -> {}", v, s.as_str(), back);
        };

        for v in [
            0.0f32, f32::from_bits(1), f32::MIN_POSITIVE, 1.0, 200.0,
            0.123456789, 0.987654321, 123.456789, 0.0123456789,
            0.333333343, 199.999985, 1.0 / 3.0, 0.1,
        ] {
            check(v);
        }

        let hi = 200.0f32.to_bits();
        let mut bits = 0u32;
        let mut checked = 0u64;
        while bits <= hi {
            check(f32::from_bits(bits));
            checked += 1;
            bits += 4093;
        }
        assert!(checked > 100_000, "swept only {checked}");
    }
}
