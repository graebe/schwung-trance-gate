#!/usr/bin/env bash
# Headless tests for the gate engine. Runs natively -- no Move required.
set -e
cd "$(dirname "$0")/.."
cc -std=c11 -Wall -Wextra -Wno-unused-parameter -Isrc/dsp \
   tests/test_gate.c src/dsp/trance_gate.c -o build/test_gate -lm
./build/test_gate || exit 1

# The UI smoke test needs the host's shared modules. They live in the sibling
# schwung worktree; skip rather than fail when it is not checked out.
SHARED="$(cd .. 2>/dev/null && pwd)/schwung/src/shared"
if [ -d "$SHARED/param_pages" ]; then
  echo
  ./build/dump_params > build/chain_params.json 2>/dev/null || \
    cc -std=c11 -Isrc/dsp tests/dump_params.c src/dsp/trance_gate.c -o build/dump_params -lm 2>/dev/null && \
    ./build/dump_params > build/chain_params.json
  TG_PARAMS=build/chain_params.json node tests/smoke_ui.mjs "$SHARED" build/.smoke
else
  echo
  echo "(ui smoke test skipped: $SHARED not found)"
fi
