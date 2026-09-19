#!/usr/bin/env bash
# Build the Trance Gate module for Schwung (ARM64).
#
# Uses Docker for cross-compilation unless CROSS_PREFIX is already set.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-module-builder"
MODULE_ID="trance-gate"

if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Trance Gate Build (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh
    echo "=== Done ==="
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
cd "$REPO_ROOT"

echo "=== Building Trance Gate ==="
echo "Cross prefix: $CROSS_PREFIX"

mkdir -p build "dist/$MODULE_ID"

echo "Compiling DSP plugin..."
${CROSS_PREFIX}gcc -Ofast -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG \
    src/dsp/trance_gate.c \
    -o "build/${MODULE_ID}.so" \
    -Isrc/dsp \
    -lm

# THE .so NAME IS LOAD-BEARING. For component_type audio_fx the chain host
# builds the path itself as modules/audio_fx/<id>/<id>.so and never reads
# module.json's "dsp" field. Name it dsp.so and the module simply does not
# load, with no error on screen -- one line in debug.log and nothing else.
echo "Packaging..."
cat src/module.json                        > "dist/$MODULE_ID/module.json"
cat "build/${MODULE_ID}.so"                > "dist/$MODULE_ID/${MODULE_ID}.so"
[ -f src/ui_chain.js ] && cat src/ui_chain.js > "dist/$MODULE_ID/ui_chain.js"
[ -f src/help.json ]   && cat src/help.json   > "dist/$MODULE_ID/help.json"
chmod +x "dist/$MODULE_ID/${MODULE_ID}.so"

cd dist
tar -czf "${MODULE_ID}-module.tar.gz" "$MODULE_ID/"
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output:  dist/$MODULE_ID/"
echo "Tarball: dist/${MODULE_ID}-module.tar.gz"
ls -la "dist/$MODULE_ID/"
