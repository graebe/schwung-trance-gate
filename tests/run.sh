#!/usr/bin/env bash
# Headless tests for the gate engine. Runs natively -- no Move required.
set -e
cd "$(dirname "$0")/.."
cc -std=c11 -Wall -Wextra -Wno-unused-parameter -Isrc/dsp \
   tests/test_gate.c src/dsp/trance_gate.c src/dsp/trance_gate_core.c -o build/test_gate -lm
./build/test_gate || exit 1

# The portable engine's own tests: sample rate, the float paths and the
# transport struct -- three freedoms the Schwung shell cannot exercise,
# because it is always 44100, always int16 and always has a host.
cc -std=c11 -Wall -Wextra -Isrc/dsp \
   tests/test_core.c src/dsp/trance_gate_core.c -o build/test_core -lm
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
GOLDEN=8e4892aa8e3947594e91cf966f7ddc98
cc -std=c11 -Wall -Wextra -Wno-unused-parameter -Isrc/dsp \
   tests/render_ref.c src/dsp/trance_gate.c src/dsp/trance_gate_core.c \
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
  cc -std=c11 -Isrc/dsp tests/dump_params.c src/dsp/trance_gate.c src/dsp/trance_gate_core.c \
     -o build/dump_params -lm
  ./build/dump_params > build/chain_params.json
  TG_PARAMS=build/chain_params.json node tests/smoke_ui.mjs "$SHARED" build/.smoke
else
  echo
  echo "(ui smoke test skipped: $SHARED not found)"
fi
