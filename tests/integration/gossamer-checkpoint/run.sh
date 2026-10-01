#!/bin/bash
# Two-step beige G3 checkpoint test (see config.mk).  Step 1 saves
# mid-boot; step 2 restores in a fresh process and must reach the same
# pixel-exact desktop the straight suite-gossamer g3dt-hd run pins.

set -e

# Set by the parent Makefile: HEADLESS_BIN, ROM_PATH, TEST_DATA,
# TEST_TMPDIR, STORAGE_CACHE

CHECKPOINT_FILE="$TEST_TMPDIR/g3.gs"

echo "Step 1: boot the 9.2.1 disk 6 G instructions, save checkpoint"
cat > "$TEST_TMPDIR/step1.script" << EOF
machine.rtc.time = "2026-10-01T12:00:00"
machine.scsi.attach_hd "$TEST_TMPDIR/hd.img" 0
scheduler.run 6000000000
assert checkpoint.save("$CHECKPOINT_FILE") "step1 checkpoint save failed"
quit
EOF

GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    model=pmg3dt ram=65536 \
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
scheduler.run 6000000000
assert debug.mac.globals.read("BootDrive") == 0x8023 "restored boot: BootDrive is not the SCSI volume"
assert debug.mac.globals.read("DSErrCode") == 0 "restored boot: a system error during startup"
machine.screen.match hd-desktop.png 0 440 20 640
quit
EOF

GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    model=pmg3dt ram=65536 \
    script="$TEST_TMPDIR/step2.script" \
    --speed=max

rm -f "$CHECKPOINT_FILE"
echo "Gossamer checkpoint test passed!"
