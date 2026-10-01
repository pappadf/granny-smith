#!/bin/bash
# Integration test: checkpoint base identity (see config.mk).
#
#   Step 1  insert a writable floppy, save a quick checkpoint, quit.
#   Step 2  a new process restores it onto the same base: accepted.
#   Step 3  the base is replaced (same path and size, other bytes, a new
#           time stamp); a new process restoring it is refused.
#
# Environment from the runner: HEADLESS_BIN, ROM_PATH, TEST_RESULTS_DIR,
# STORAGE_CACHE, TEST_VAR_ARGS.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CP_DIR="$TEST_RESULTS_DIR/cp"

# Run one step's script in a fresh process.
step() {
    # shellcheck disable=SC2086 — TEST_VAR_ARGS is intentionally word-split
    GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
        rom="$ROM_PATH" \
        script="$SCRIPT_DIR/$1" \
        --checkpoint-dir="$CP_DIR" \
        --var TEST_RESULTS_DIR="$TEST_RESULTS_DIR" \
        $TEST_VAR_ARGS \
        --speed=turbo
}

cd "$TEST_RESULTS_DIR"
echo "Step 1: save a quick checkpoint with a writable floppy"
step step1.script
echo "Step 2: restore it onto the same base"
step step2.script
echo "Step 3: replace the base, then restore"
cp "$TEST_RESULTS_DIR/b.dsk" "$TEST_RESULTS_DIR/a.dsk"
out=$(step step3.script 2>&1) || { echo "$out"; exit 1; }
echo "$out"
echo "$out" | grep -q "is not the image the checkpoint was saved with" || {
    echo "ERROR: the refusal did not name the replaced base"
    exit 1
}
echo "Checkpoint base identity test passed"
