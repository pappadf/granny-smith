# Integration test: Quadra 900 checkpoint save/restore across processes
# (proposal-machine-quadra-700-900-950.md Phase I)
#
# Step 1 boots the tower HD image halfway (mid-extension load: the IOP
# mailboxes, Caboose, dual 53C96, SONIC and DAFB all carry live state) and
# saves a consolidated checkpoint.  Step 2 restores it in a FRESH process
# and finishes the boot; the desktop must pixel-match the same golden the
# straight q900-boot-hd run pins.  This exercises the whole mcu save set —
# the tower devices included — and the save/restore stream ordering.

TEST_NAME := Quadra 900 checkpoint save/restore
TEST_DESC := Tower save-state: mid-boot save, cross-process restore, desktop pixel-match

TEST_ROM := roms/q700-q900-420dbff3.rom

# Copy the HD image into TEST_TMPDIR: the System writes to the volume during
# boot, and the consolidated restore recreates the backing file in place.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_1_20mb_24ac_cd_32bit.img" "$(TEST_TMPDIR)/hd.img"

# Two-step custom runner (save process + restore process).
TEST_RUNNER := run.sh

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
