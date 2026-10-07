#!/bin/bash
# Integration test: checkpoint wrapped volume (see config.mk).
#
#   Step 1  attach a bare HFS volume as the hard disk, save a consolidated
#           checkpoint, quit.
#   Step 2  a new process restores it with the image file present.
#   Step 3  the image file is deleted; a new process restores it again.
#
# Both restores must see the disk at the size it had when saved, wrapper
# included.
#
# Environment from the runner: HEADLESS_BIN, ROM_PATH, TEST_RESULTS_DIR,
# STORAGE_CACHE, TEST_VAR_ARGS.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Run one step's script in a fresh process; print the SIZE= it reports.
step() {
    local out
    # shellcheck disable=SC2086 — TEST_VAR_ARGS is intentionally word-split
    out=$(GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
        rom="$ROM_PATH" \
        script="$SCRIPT_DIR/$1" \
        --checkpoint-dir="$TEST_RESULTS_DIR/cp" \
        --var TEST_RESULTS_DIR="$TEST_RESULTS_DIR" \
        $TEST_VAR_ARGS \
        --speed=turbo 2>&1) || { echo "$out" >&2; return 1; }
    echo "$out" >&2
    echo "$out" | sed -n 's/^SIZE=//p' | tail -1
}

cd "$TEST_RESULTS_DIR"
bare=$(stat -c%s "$TEST_RESULTS_DIR/vol.dsk")
echo "Step 1: attach a bare volume as the hard disk and save a consolidated checkpoint"
saved=$(step step1.script)
echo "Step 2: restore it with the image file present"
present=$(step step2.script)
echo "Step 3: delete the image file, then restore"
rm -f "$TEST_RESULTS_DIR/vol.dsk"
missing=$(step step2.script)

echo "disk size: bare volume=$bare, saved=$saved, restored with image=$present, restored without image=$missing"
if [ -z "$saved" ] || [ "$saved" -le "$bare" ]; then
    echo "ERROR: the volume was not wrapped when attached"
    exit 1
fi
if [ "$present" != "$saved" ] || [ "$missing" != "$saved" ]; then
    echo "ERROR: a restore did not bring the disk back wrapped"
    exit 1
fi
echo "Checkpoint wrapped volume test passed"
