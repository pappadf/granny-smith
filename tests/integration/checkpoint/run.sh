#!/bin/bash
# Integration test: Checkpoint Save/Load
# This script runs the emulator twice to test checkpoint functionality:
#   Step 1: Boot from floppy, run 50M instructions, save checkpoint, quit
#   Step 2: Boot from SCSI HD, run 100M instructions, load checkpoint, run 50M more, verify desktop

set -e

# These variables are set by the parent Makefile
# HEADLESS_BIN, ROM_PATH, TEST_DATA, TEST_TMPDIR, STORAGE_CACHE

CHECKPOINT_FILE="$TEST_TMPDIR/checkpoint.gs"
FD_IMAGE="$TEST_TMPDIR/System_6_0_8.dsk"
HD_IMAGE="$TEST_TMPDIR/hd1.img"

# Copy floppy image to temp directory so consolidated checkpoint restore
# (which recreates backing files in-place) does not overwrite the original.
cp "$TEST_DATA/systems/System_6_0_8.dsk" "$FD_IMAGE"

# Export checkpoint path for scripts to use
export CHECKPOINT_FILE

echo "Step 1: Boot from floppy, run 50M instructions, save checkpoint"
echo "Checkpoint will be saved to: $CHECKPOINT_FILE"

# Create step1 script - boot from floppy, run 50M instructions, save checkpoint.
# `\$(...)` escapes bash command substitution so the literal `$(...)`
# reaches the gs-headless expression evaluator.
cat > "$TEST_TMPDIR/step1.script" << INNER_EOF
# Step 1: Boot from floppy and save checkpoint
run 50000000
assert \${checkpoint_save("$CHECKPOINT_FILE")} "step1 checkpoint save failed"
quit
INNER_EOF

GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    fd="$FD_IMAGE" \
    script="$TEST_TMPDIR/step1.script" \
    --speed=max

if [ ! -f "$CHECKPOINT_FILE" ]; then
    echo "ERROR: Checkpoint file not created: $CHECKPOINT_FILE"
    exit 1
fi
echo "Checkpoint saved: $CHECKPOINT_FILE ($(stat -c%s "$CHECKPOINT_FILE") bytes)"

echo ""
echo "Step 2: Boot from SCSI HD, load checkpoint, run 50M more, verify desktop"

# Create step2 script - boot from SCSI, load checkpoint, run 50M more, verify screen.
# `\$(...)` escapes bash command substitution so the literal `$(...)`
# reaches the gs-headless expression evaluator.
cat > "$TEST_TMPDIR/step2.script" << INNER_EOF
# Step 2: Boot from SCSI HD, load checkpoint, continue to desktop
run 100000000
assert \${checkpoint_load("$CHECKPOINT_FILE")} "step2 checkpoint load failed"
# Run 50M more instructions (should reach desktop just like step 1 would)
run 50000000
# Save screenshot for debugging, then verify
screen.match desktop.png
quit
INNER_EOF

GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    hd="$HD_IMAGE" \
    script="$TEST_TMPDIR/step2.script" \
    --speed=max

echo "Checkpoint test passed!"
