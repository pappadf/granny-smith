# Integration test: hd= on a Lisa attaches the ProFile (M3).
#
# The Lisa's hard disk is the parallel-port ProFile, not SCSI.  hd= used to
# hand the SCSI layer the Lisa's NULL bus and crash the harness (N-01); it
# now goes through the model's hard-disk bay like every other front end.

TEST_NAME := Media bays (Lisa)
TEST_DESC := hd= on a Lisa attaches the ProFile instead of crashing; cdrom= is refused

TEST_ROM := roms/lisa2-revh-098917b2.rom

# 9728 blocks of 532 bytes: a 5 MB ProFile.
TEST_SETUP := truncate -s 5175296 "$(WORK_DIR)/pro.img"
TEST_ARGS := model=lisa hd=$(WORK_DIR)/pro.img

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
