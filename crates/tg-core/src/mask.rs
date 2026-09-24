/*!
The step and tie bitmasks.

128 steps do not fit a machine word. In C this is `struct tg_mask_t { uint32_t
w[4]; }`, deliberately wrapped so that a bare integer's `>>` could not be used
on it by accident -- a bare `uint32_t` still compiles with `>> 40` and quietly
drops every step past 31, and the audit for that was a comment asking who
touches `.steps` directly.

Here the wrapper does the work the comment was doing: the words are private
and the only ways in are `get`, `set` and the hex conversions, so reaching
past them is not something a caller can express.
*/

use crate::MAX_STEPS;

pub const MASK_WORDS: usize = (MAX_STEPS + 31) / 32;

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Mask {
    w: [u32; MASK_WORDS],
}

impl Mask {
    pub const fn new() -> Self {
        Self { w: [0; MASK_WORDS] }
    }

    #[inline]
    pub fn get(&self, i: usize) -> bool {
        i < MAX_STEPS && (self.w[i >> 5] >> (i & 31)) & 1 != 0
    }

    #[inline]
    pub fn set(&mut self, i: usize, on: bool) {
        if i >= MAX_STEPS {
            return;
        }
        let bit = 1u32 << (i & 31);
        if on {
            self.w[i >> 5] |= bit;
        } else {
            self.w[i >> 5] &= !bit;
        }
    }

    pub fn clear(&mut self) {
        self.w = [0; MASK_WORDS];
    }

    /// Parse the engine's hex form: the highest non-zero word first and
    /// unpadded, then each lower word as exactly eight digits. So a <=32-step
    /// pattern is the same eight-digit string it always was, and parsing runs
    /// from the RIGHT in eight-digit chunks. Reading left to right would put
    /// word 0 in the wrong place for every pattern longer than 32 steps and
    /// leave short ones looking perfectly correct.
    pub fn from_hex(hex: &str) -> Self {
        let mut m = Self::new();
        let b = hex.as_bytes();
        let mut end = b.len();
        let mut k = 0usize;
        while k < MASK_WORDS && end > 0 {
            let start = end.saturating_sub(8);
            let mut v: u32 = 0;
            for &c in &b[start..end] {
                let d = match c {
                    b'0'..=b'9' => (c - b'0') as u32,
                    b'a'..=b'f' => (c - b'a') as u32 + 10,
                    b'A'..=b'F' => (c - b'A') as u32 + 10,
                    /* Anything else ends the number, as strtoul would. */
                    _ => break,
                };
                v = v.wrapping_mul(16).wrapping_add(d);
            }
            m.w[k] = v;
            end = start;
            k += 1;
        }
        m
    }

    /// The minimal hex the state and the `ui` readout both use: the highest
    /// non-zero word unpadded, every lower word in full. Writes into `out`
    /// and returns the length, matching the C's snprintf contract.
    pub fn to_hex(&self, out: &mut dyn core::fmt::Write) -> core::fmt::Result {
        let mut top = MASK_WORDS - 1;
        while top > 0 && self.w[top] == 0 {
            top -= 1;
        }
        write!(out, "{:X}", self.w[top])?;
        for k in (0..top).rev() {
            write!(out, "{:08X}", self.w[k])?;
        }
        Ok(())
    }
}
