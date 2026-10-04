#!/bin/bash
# Gossamer failed-restore test (see config.mk).  Step 1 saves a checkpoint of
# a bare G3; it is cut short by a few bytes, so a restore fails in the last
# block, after the new machine's ATA cells exist.  Step 2 boots Mac OS 9.2.1
# with a second disk on ATA, fails that restore mid-boot, and must finish the
# boot on the machine that kept running.

set -e

# Set by the parent Makefile: HEADLESS_BIN, ROM_PATH, TEST_TMPDIR, STORAGE_CACHE

CHECKPOINT_FILE="$TEST_TMPDIR/g3.gs"
TRUNCATED_FILE="$TEST_TMPDIR/g3-truncated.gs"

echo "Step 1: save a checkpoint of a bare G3"
cat > "$TEST_TMPDIR/step1.script" << EOF
scheduler.run 100000000
assert checkpoint.save("$CHECKPOINT_FILE") "step1 checkpoint save failed"
quit
EOF

GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    model=pmg3dt ram=65536 \
    script="$TEST_TMPDIR/step1.script" \
    --speed=turbo

# The stream ends in the event queue, which is restored after the whole
# machine -- the ATA cells included -- has been built.
cp "$CHECKPOINT_FILE" "$TRUNCATED_FILE"
truncate -s -8 "$TRUNCATED_FILE"

echo ""
echo "Step 2: boot 9.2.1 with an ATA disk, fail the restore, finish the boot"
cat > "$TEST_TMPDIR/step2.script" << EOF
assert files.hd_create("$TEST_TMPDIR/ata.img", "HD500SC") "create the ATA disk"
machine.rtc.time = "2026-10-01T12:00:00"
machine.scsi.attach_hd "$TEST_TMPDIR/hd.img" 0
assert machine.ata.attach_hd("$TEST_TMPDIR/ata.img", 0) "attach the ATA disk"
scheduler.run 1000000000
assert !checkpoint.load("$TRUNCATED_FILE") "the truncated checkpoint loaded"
assert machine.ata.devices == "hd - - -" "the running machine lost its ATA disk"
let ata_reads = files.images[1].reads
scheduler.run 11000000000
assert files.images[1].reads > \$ata_reads "the running machine did no ATA I/O after the failed restore"
assert debug.mac.globals.read("BootDrive") == 0x8023 "BootDrive is not the SCSI volume"
assert debug.mac.globals.read("DSErrCode") == 0 "a system error during startup"
quit
EOF

GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    model=pmg3dt ram=65536 \
    script="$TEST_TMPDIR/step2.script" \
    --speed=turbo

rm -f "$CHECKPOINT_FILE" "$TRUNCATED_FILE"
echo "Gossamer failed-restore test passed!"
