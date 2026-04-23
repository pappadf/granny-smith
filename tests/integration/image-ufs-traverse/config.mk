# Integration test: Phase 3 UFS read + cp -r
#
# Exercises the UFS walker against the A/UX 3.0.1 retail ISO.  Partition 6
# ("UNIX Root&Usr slice 0") is the root filesystem; we list several well-
# known paths and copy the /etc subtree out to WORK_DIR.

TEST_NAME := Image UFS traverse
TEST_DESC := Descends into the A/UX 3.0.1 UFS root partition and copies out /etc

# Universal ROM shared by SE/30, IIcx, IIx
TEST_ROM := roms/SE30.rom
