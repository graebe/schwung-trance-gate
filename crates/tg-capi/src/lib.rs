/*!
The `tg_core_*` C ABI.

Fourteen symbols, byte-for-byte the surface the C engine exported, so every
existing caller -- the Schwung shell, the JUCE plugin, and 1,510 lines of C
tests -- links this instead without changing a line. That is the whole point:
the tests are not rewritten for the port, they are relinked, and they are what
says the port is correct.

# Safety

Every function here is `unsafe` in the C sense and safe in practice under the
contract the C had: a `tg_core_t*` is a pointer returned by
[`tg_core_create`] and not yet destroyed, and buffers are valid for the frame
counts given. Null is checked because the C checked it, and callers rely on
that; anything else is the caller's bargain, as it was before.
*/

use std::ffi::{c_char, c_int, CStr};
use tg_core::params::Param;
use tg_core::{Instance, Transport};

/// Opaque to C, exactly as `tg_core_t` was.
pub struct TgCore(Instance);

#[repr(C)]
pub struct TgTransport {
    pub running: c_int,
    pub beats: f64,
    pub bpm: f32,
}

/// A `*const c_char` as a `&str`, or "" -- which is what the C's `atof` and
/// `strcmp` effectively did with junk. Invalid UTF-8 is treated as absent
/// rather than panicking: this runs on an audio callback.
unsafe fn s<'a>(p: *const c_char) -> &'a str {
    if p.is_null() {
        return "";
    }
    CStr::from_ptr(p).to_str().unwrap_or("")
}

#[no_mangle]
pub extern "C" fn tg_core_create(sample_rate: f64) -> *mut TgCore {
    Box::into_raw(Box::new(TgCore(Instance::new(sample_rate))))
}

#[no_mangle]
pub unsafe extern "C" fn tg_core_destroy(c: *mut TgCore) {
    if !c.is_null() {
        drop(Box::from_raw(c));
    }
}

#[no_mangle]
pub unsafe extern "C" fn tg_core_set_sample_rate(c: *mut TgCore, sample_rate: f64) {
    let Some(c) = c.as_mut() else { return };
    if sample_rate <= 0.0 {
        return;
    }
    c.0.sample_rate = sample_rate;
    /* ms_per_step cancels the sample rate out, so this changes nothing today.
     * It is here so that "ms_per_step is current" holds at every door into
     * the struct rather than at the two that happen to matter. */
    c.0.recalc_ms_per_step();
}

#[no_mangle]
pub unsafe extern "C" fn tg_core_get_sample_rate(c: *const TgCore) -> f64 {
    c.as_ref().map_or(0.0, |c| c.0.sample_rate)
}

#[no_mangle]
pub unsafe extern "C" fn tg_core_set_param(
    c: *mut TgCore,
    key: *const c_char,
    val: *const c_char,
) {
    let Some(c) = c.as_mut() else { return };
    if key.is_null() || val.is_null() {
        return;
    }
    c.0.set_param(s(key), s(val));
}

/// `tg_core_set_num`: the twelve automatable values by number, for host
/// automation arriving on the audio thread. See [`tg_core::params::Param`] --
/// the discriminants are the ABI, so a host that saved an automation lane
/// saved these integers.
///
/// A parameter outside the enum is DROPPED, not clamped onto a neighbour: one
/// silently moving a different control is worse than one doing nothing.
#[no_mangle]
pub unsafe extern "C" fn tg_core_set_num(c: *mut TgCore, param: c_int, value: f64) {
    let Some(c) = c.as_mut() else { return };
    let Some(p) = Param::from_i32(param) else { return };
    c.0.set_num(p, value);
}

/// Returns the length written, or -1 for a key this engine does not serve --
/// which is how a shell knows to answer its own.
#[no_mangle]
pub unsafe extern "C" fn tg_core_get_param(
    c: *mut TgCore,
    key: *const c_char,
    buf: *mut c_char,
    buf_len: c_int,
) -> c_int {
    let Some(c) = c.as_ref() else { return -1 };
    if key.is_null() || buf.is_null() || buf_len <= 0 {
        return -1;
    }
    let out = std::slice::from_raw_parts_mut(buf as *mut u8, buf_len as usize);
    c.0.get_param(s(key), out)
}

#[no_mangle]
pub unsafe extern "C" fn tg_core_on_midi(_c: *mut TgCore, _msg: *const u8, _len: c_int) {
    /* The engine has never used MIDI; the Move shell claims CCs for the
     * arrows and handles them in its UI layer. Kept because it is part of the
     * published surface. */
}

unsafe fn transport(t: *const TgTransport) -> Option<Transport> {
    t.as_ref().map(|t| Transport {
        running: t.running != 0,
        beats: t.beats,
        bpm: t.bpm,
    })
}

#[no_mangle]
pub unsafe extern "C" fn tg_core_process_i16(
    c: *mut TgCore,
    lr: *mut i16,
    frames: c_int,
    t: *const TgTransport,
) {
    let Some(c) = c.as_mut() else { return };
    if lr.is_null() || frames <= 0 {
        return;
    }
    let buf = std::slice::from_raw_parts_mut(lr, frames as usize * 2);
    c.0.process_i16(buf, frames as usize, transport(t).as_ref());
}

#[no_mangle]
pub unsafe extern "C" fn tg_core_process_f32(
    c: *mut TgCore,
    lr: *mut f32,
    frames: c_int,
    t: *const TgTransport,
) {
    let Some(c) = c.as_mut() else { return };
    if lr.is_null() || frames <= 0 {
        return;
    }
    let buf = std::slice::from_raw_parts_mut(lr, frames as usize * 2);
    c.0.process_f32(buf, frames as usize, transport(t).as_ref());
}

#[no_mangle]
pub unsafe extern "C" fn tg_core_process_f32_split(
    c: *mut TgCore,
    l: *mut f32,
    r: *mut f32,
    frames: c_int,
    t: *const TgTransport,
) {
    let Some(c) = c.as_mut() else { return };
    if l.is_null() || r.is_null() || frames <= 0 {
        return;
    }
    let n = frames as usize;
    let lb = std::slice::from_raw_parts_mut(l, n);
    let rb = std::slice::from_raw_parts_mut(r, n);
    c.0.process_f32_split(lb, rb, n, transport(t).as_ref());
}

#[no_mangle]
pub unsafe extern "C" fn tg_core_phase01(c: *const TgCore) -> f64 {
    c.as_ref().map_or(0.0, |c| c.0.phase01())
}

/*
 * THE SHAPES, VISIBLE TO THE TESTS.
 *
 * The properties the whole envelope rests on -- endpoints, monotonicity, an
 * exact inverse -- are worth asserting directly rather than inferred from
 * rendered audio, where a broken shape would show up as "the gate sounds
 * odd".
 */
#[no_mangle]
pub extern "C" fn tg_test_shape(curve: c_int, t: f64) -> f64 {
    tg_core::envelope::shape(tg_core::envelope::Curve::from_i32(curve), t)
}

#[no_mangle]
pub extern "C" fn tg_test_shape_inv(curve: c_int, w: f64) -> f64 {
    tg_core::envelope::shape_inv(tg_core::envelope::Curve::from_i32(curve), w)
}

/// For the Move shell, which needs the same `Instance` without going back out
/// through C.
impl TgCore {
    pub fn inner(&mut self) -> &mut Instance {
        &mut self.0
    }
}
