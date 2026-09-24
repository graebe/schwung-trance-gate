#!/usr/bin/env bash
# Headless tests for the gate engine. Runs natively -- no Move required.
#
# THE ENGINE IS RUST AND THE TESTS ARE STILL C. That is deliberate and is the
# whole verification strategy of the port: these suites link the engine
# through its C ABI and do not care what is behind it, so they were relinked
# rather than rewritten -- 1,510 lines of existing assertions, unchanged,
# saying whether the port is correct.
set -e
cd "$(dirname "$0")/.."

# rustup's shims may not be on PATH; the toolchain's own bin always is once
# found, and cargo needs rustc beside it.
if ! command -v cargo >/dev/null 2>&1; then
    TC="$(rustup which cargo 2>/dev/null)" || true
    [ -n "$TC" ] && PATH="$(dirname "$TC"):$PATH" && export PATH
fi
cargo build --release -p tg-capi
ENGINE=target/release/libtg_capi.a
cc -std=c11 -Wall -Wextra -Wno-unused-parameter -Isrc/dsp \
   tests/test_gate.c src/dsp/trance_gate.c "$ENGINE" -o build/test_gate -lm
./build/test_gate || exit 1

# The portable engine's own tests: sample rate, the float paths and the
# transport struct -- three freedoms the Schwung shell cannot exercise,
# because it is always 44100, always int16 and always has a host.
cc -std=c11 -Wall -Wextra -Isrc/dsp \
   tests/test_core.c "$ENGINE" -o build/test_core -lm
./build/test_core || exit 1

# THE GOLDEN RENDER. 20 seconds of audio through the whole engine, compared
# by hash against a render captured before the engine was ever split out of
# the Schwung module.
#
# This is the one check that the SOUND has not changed, and it is the reason
# the 32-bit-mask widening to 128 steps could be done at all: a step shifted
# by one position, an envelope restarted a sample early, a rate table entry
# INSERTED rather than appended -- none of those fail a unit test, and all of
# them fail here. It was being run by hand, which is the same as not being
# run; a check nobody is obliged to remember is not a check.
#
# If this fires and the change to the audio was DELIBERATE, re-record the
# hash with `tests/render_ref > /tmp/ref.raw` and say so in the commit.
# Re-recorded 2026-09-23 when the per-step level became latched at gate-open.
# The diff was confined to steps 2 and 6 -- the steps FOLLOWING the golden
# patch's ties at 1 and 5, where the level used to jump mid-gate to the new
# step's amount. That jump was the bug; everything else is byte-identical.
# Previous: b208becc62657c9748247b9daa7b0362
# Re-recorded 2026-09-23 when the envelope's stages became a percentage of the
# gate's WIDTH rather than milliseconds. The patch is the same patch in the
# new units (3.5 ms at a 91.46 ms width is 3.8267%), and the render was
# compared NUMERICALLY before this was accepted: 554 of 176,400 frames differ
# and never by more than 4 LSB of 32768, which is the rounding in four decimal
# places of a percentage. A units change cannot keep a hash; it can and must
# keep the sound.
# Previous: 8e4892aa8e3947594e91cf966f7ddc98
# Re-recorded 2026-09-24 for the Rust port -- and the OLD HASH WAS PINNING A
# COMPILER, not the algorithm.
#
# The C engine compiled with clang's default fp-contract fuses `a - b*c` into
# a single FMA, one rounding instead of two. Rust does not contract, so the
# two differed by one f32 ulp wherever Amount or a per-step level was not 1 --
# 140 of 352,800 samples, never by more than 1 LSB of 32768.
#
# Built with -ffp-contract=off the C produces THIS hash exactly, which is what
# identified the cause and what makes the new number the algorithm's rather
# than a build flag's. Worth knowing: the shipped Move .so is built -Ofast,
# which contracts harder still, so the module and its tests never agreed
# bit-for-bit until now.
# Previous: 4264807b9e7da87844309fa48d0cc8a3 (C, with contraction)
GOLDEN=3992810c52d7962b4d25b3a30494ee2e
cc -std=c11 -Wall -Wextra -Wno-unused-parameter -Isrc/dsp \
   tests/render_ref.c src/dsp/trance_gate.c "$ENGINE" \
   -o build/render_ref -lm
GOT=$(./build/render_ref | md5 -q 2>/dev/null || ./build/render_ref | md5sum | cut -d" " -f1)
echo
echo "golden render:"
if [ "$GOT" = "$GOLDEN" ]; then
  echo "  20s reference render is bit-identical                      ok"
else
  echo "  RENDER CHANGED: got $GOT want $GOLDEN"
  exit 1
fi

# The UI smoke test needs the host's shared modules. They live in the sibling
# schwung worktree; skip rather than fail when it is not checked out.
SHARED="$(cd .. 2>/dev/null && pwd)/schwung/src/shared"
if [ -d "$SHARED/param_pages" ]; then
  echo
  # ALWAYS REBUILD. This used to try the existing binary first and only
  # compile if running it failed -- so an edit to chain_params was tested
  # against the PREVIOUS build's JSON, silently, for as long as the old
  # binary kept working. A stale fixture reports the old contract as the
  # current one, which is worse than no fixture at all.
  cc -std=c11 -Isrc/dsp tests/dump_params.c src/dsp/trance_gate.c "$ENGINE" \
     -o build/dump_params -lm
  ./build/dump_params > build/chain_params.json
  TG_PARAMS=build/chain_params.json node tests/smoke_ui.mjs "$SHARED" build/.smoke
else
  echo
  echo "(ui smoke test skipped: $SHARED not found)"
fi
