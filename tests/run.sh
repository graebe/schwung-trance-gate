#!/usr/bin/env bash
# Headless tests for the gate engine. Runs natively -- no Move required.
set -e
cd "$(dirname "$0")/.."
cc -std=c11 -Wall -Wextra -Wno-unused-parameter -Isrc/dsp \
   tests/test_gate.c src/dsp/trance_gate.c -o build/test_gate -lm
exec ./build/test_gate
