#!/bin/bash
# Two-step Quadra 900 checkpoint test (see config.mk).  Step 1 saves
# mid-boot; step 2 restores in a fresh process and must reach the same
# pixel-exact desktop the straight q900-boot-hd run pins.

set -e

# Set by the parent Makefile: HEADLESS_BIN, ROM_PATH, TEST_DATA,
# TEST_TMPDIR, STORAGE_CACHE

CHECKPOINT_FILE="$TEST_TMPDIR/q900.gs"

echo "Step 1: boot the tower HD image 500M instructions, save checkpoint"
cat > "$TEST_TMPDIR/step1.script" << EOF
# Mid-boot: extensions loading; IOPs/Caboose/53C96 carry live state.
scheduler.run 500000000
assert debug.mac.globals.read("MMUFluff") == 14 "not identified as Quadra 900"
assert checkpoint.save("$CHECKPOINT_FILE") "step1 checkpoint save failed"
quit
EOF

GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    model=q900 ram=8192 \
    hd="$TEST_TMPDIR/hd.img" \
    script="$TEST_TMPDIR/step1.script" \
    --speed=max

if [ ! -f "$CHECKPOINT_FILE" ]; then
    echo "ERROR: Checkpoint file not created: $CHECKPOINT_FILE"
    exit 1
fi
echo "Checkpoint saved: $CHECKPOINT_FILE ($(stat -c%s "$CHECKPOINT_FILE") bytes)"

echo ""
echo "Step 2: restore in a fresh process, finish the boot, match the golden"
cat > "$TEST_TMPDIR/step2.script" << EOF
assert checkpoint.load("$CHECKPOINT_FILE") "step2 checkpoint load failed"
# Finish the boot: the desktop is stable from ~800M in the straight run;
# the extra margin absorbs restore-time scheduler drift.
scheduler.run 500000000
assert debug.mac.globals.read("MMUFluff") == 14 "restored machine is not a Quadra 900"
assert machine.screen.width == 640 "not in 640x480 mode"
assert machine.screen.height == 480 "not in 640x480 mode"
machine.screen.match ../q900-boot-hd/hd-desktop.png
quit
EOF

GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    model=q900 ram=8192 \
    script="$TEST_TMPDIR/step2.script" \
    --speed=max

echo "Quadra 900 checkpoint test passed!"
