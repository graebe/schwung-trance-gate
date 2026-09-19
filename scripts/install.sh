#!/usr/bin/env bash
# Install the Trance Gate module onto a Move running Schwung.
#
#   ./scripts/install.sh                      # ableton@move.local
#   MOVE_HOST=ableton@192.168.1.42 ./scripts/install.sh
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="trance-gate"
HOST="${MOVE_HOST:-ableton@move.local}"
BASE="/data/UserData/schwung"
DEST="$BASE/modules/audio_fx/${MODULE_ID}"

cd "$REPO_ROOT"

if [ ! -d "dist/$MODULE_ID" ]; then
    echo "Error: dist/$MODULE_ID not found. Run ./scripts/build.sh first." >&2
    exit 1
fi

echo "=== Installing Trance Gate to $HOST ==="

# FAIL RATHER THAN CREATE THE PATH. mkdir -p on a wrong base happily invents
# /data/UserData/schwung on a device that keeps its modules somewhere else
# (an older install used .../move-anything), and the result is a successful
# scp, no error anywhere, and a module that never appears in the FX picker.
if ! ssh "$HOST" "[ -d '$BASE/modules' ]"; then
    echo "Error: $BASE/modules does not exist on $HOST." >&2
    echo "Is Schwung installed there? Check with:" >&2
    echo "  ssh $HOST 'ls -d /data/UserData/*/modules'" >&2
    exit 1
fi

ssh "$HOST" "mkdir -p '$DEST'"
scp -q -r "dist/$MODULE_ID/"* "$HOST:$DEST/"

# Readable/writable so schwung-manager can update it later.
ssh "$HOST" "chmod -R a+rw '$DEST' && chmod +x '$DEST/${MODULE_ID}.so'"

echo ""
echo "Installed to: $DEST"
ssh "$HOST" "ls -la '$DEST'"
echo ""
echo "No restart needed -- the FX picker scans the modules dir each time it opens."
echo "If the module is ALREADY loaded in a slot, that slot keeps the old .so:"
echo "set the position to None and pick it again, or reboot."
