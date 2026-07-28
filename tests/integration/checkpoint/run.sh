#!/bin/bash
# Integration test: Checkpoint save/load across processes (see config.mk).
#
#   Step 1  boot the 6.0.8 floppy to the desktop, save a checkpoint, quit.
#   Step 2  boot the SCSI HD (a different device set), load step 1's
#           checkpoint, re-match the floppy desktop, then drive the Apple
#           menu to prove the restored machine is interactive.
#
# Both steps run from TEST_RESULTS_DIR (media + checkpoint live there, so
# nothing is written beside tests/data) while the scripts stay in the test
# directory: their goldens resolve through $TEST_DIR and the shared
# library through the script's own directory.
#
# Environment from the runner: HEADLESS_BIN, ROM_PATH, TEST_DATA,
# TEST_RESULTS_DIR, STORAGE_CACHE, TEST_VAR_ARGS.

set -e

CHECKPOINT_FILE="$TEST_RESULTS_DIR/checkpoint.gs"
FD_IMAGE="$TEST_RESULTS_DIR/System_6_0_8.dsk"
HD_IMAGE="$TEST_RESULTS_DIR/hd1.img"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for f in "$FD_IMAGE" "$HD_IMAGE"; do
    [ -f "$f" ] || { echo "ERROR: test media missing: $f"; exit 1; }
done

echo "Step 1: boot the floppy to the desktop and save a checkpoint"
echo "  checkpoint: $CHECKPOINT_FILE"

# fd= opens the floppy writable, matching the conditions the goldens were
# captured under.
cd "$TEST_RESULTS_DIR"
# shellcheck disable=SC2086 — TEST_VAR_ARGS is intentionally word-split
GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    fd="$FD_IMAGE" \
    script="$SCRIPT_DIR/step1.script" \
    --var TEST_DIR="$SCRIPT_DIR" \
    --var TEST_RESULTS_DIR="$TEST_RESULTS_DIR" \
    $TEST_VAR_ARGS \
    --speed=max

if [ ! -f "$CHECKPOINT_FILE" ]; then
    echo "ERROR: Checkpoint file not created: $CHECKPOINT_FILE"
    exit 1
fi
echo "Checkpoint saved: $CHECKPOINT_FILE ($(stat -c%s "$CHECKPOINT_FILE") bytes)"

echo ""
echo "Step 2: boot the SCSI HD, restore the floppy checkpoint over it"

cd "$TEST_RESULTS_DIR"
# shellcheck disable=SC2086 — TEST_VAR_ARGS is intentionally word-split
GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    hd="$HD_IMAGE" \
    script="$SCRIPT_DIR/step2.script" \
    --var TEST_DIR="$SCRIPT_DIR" \
    --var TEST_RESULTS_DIR="$TEST_RESULTS_DIR" \
    $TEST_VAR_ARGS \
    --speed=max

echo "Checkpoint test passed"
echo "Results saved to: $TEST_RESULTS_DIR"
