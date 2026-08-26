# Integration test: TNT MESH disk boot on the pm8500 (604)
#
# The tnt-hd-boot chain (driver match through System 7.6 to the Finder)
# on the 604 machine that the boot wall used to kill: the 604 model's
# hardware-split path for misaligned page-crossing stores dropped the
# update-form rA writeback, shearing the 68k emulator's A7 during the
# boot-time Name-Registry import (pinned FE03 workspace at $3FF040,
# clobbered return address, runaway into zeroed RAM).  This row holds
# the fix: the same disk image boots to the Finder on the 604.

TEST_NAME := TNT MESH disk boot (pm8500/604)
TEST_DESC := pm8500 + 7.6 disk on the MESH bus: the 604 boot wall stays down — System 7.6 to the Finder

TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

# Copied into WORK_DIR because the System writes to the volume during boot.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_6_170mb_24ac.img" "$(WORK_DIR)/hd.img"

TEST_ARGS := model=pm8500 ram=32768

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
