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
