#!/bin/bash
# Integration test: Checkpoint Save/Load v2
# This script runs the emulator twice to test checkpoint functionality:
#   Step 1: Boot from floppy, run 50M instructions, save checkpoint, quit
#   Step 2: Boot from SCSI HD, run 100M instructions, load checkpoint, run 50M more, verify desktop
#
# Unlike the original checkpoint test, this version:
#   - Uses static script files (step1.script, step2.script) - no auto-generation
#   - Uses a persistent test-results directory instead of /tmp for easier debugging
#   - Runs emulator from test-results dir so scripts can use relative paths

set -e

# These variables are set by the parent Makefile
# HEADLESS_BIN, ROM_PATH, TEST_DATA, STORAGE_CACHE, TEST_RESULTS_DIR

CHECKPOINT_FILE="$TEST_RESULTS_DIR/checkpoint.gs"
FD_IMAGE="$TEST_RESULTS_DIR/System_6_0_8.dsk"
HD_IMAGE="$TEST_RESULTS_DIR/hd1.img"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "Step 1: Boot from floppy, run 50M instructions, save checkpoint"
echo "Checkpoint will be saved to: $CHECKPOINT_FILE"
echo "Test results directory: $TEST_RESULTS_DIR"

# Run step 1 from TEST_RESULTS_DIR so the script can use relative paths.
# Floppy is passed via fd= (not insert-fd) so the image is opened writable,
# matching the conditions the desktop.png baseline was captured under.
cd "$TEST_RESULTS_DIR"
GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    fd="$FD_IMAGE" \
    script="$SCRIPT_DIR/step1.script" \
    --speed=max

if [ ! -f "$CHECKPOINT_FILE" ]; then
    echo "ERROR: Checkpoint file not created: $CHECKPOINT_FILE"
    exit 1
fi
echo "Checkpoint saved: $CHECKPOINT_FILE ($(stat -c%s "$CHECKPOINT_FILE") bytes)"

echo ""
echo "Step 2: Boot from SCSI HD, load checkpoint, run 50M more, verify desktop"

# Run step 2 from TEST_RESULTS_DIR so the script can use relative paths
cd "$TEST_RESULTS_DIR"
GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    script="$SCRIPT_DIR/step2.script" \
    --speed=max

echo "Checkpoint test passed!"
echo "Results saved to: $TEST_RESULTS_DIR"

