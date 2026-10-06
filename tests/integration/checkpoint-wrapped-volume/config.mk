# Integration test: a consolidated checkpoint of a wrapped volume restores
# it wrapped, even when the image file is gone.
#
# A bare HFS volume attached as a hard disk is wrapped: a partition map and
# the GSDisk driver go in front of it, and the guest addresses the disk
# through them.  A consolidated checkpoint carries every block, so it can be
# opened where its image file no longer exists (another browser, the disk
# deleted since); the restore then puts a zero-filled placeholder there.
# The wrapper used to be rebuilt by sniffing that placeholder before the
# blocks were restored, found no volume, and left the disk unwrapped: the
# guest saw every block shifted ("The System file on this startup disk may
# be damaged").

TEST_NAME := Checkpoint wrapped volume
TEST_DESC := A consolidated checkpoint restores a wrapped volume wrapped, with or without its image file

TEST_ROM := roms/plus-v3-4d1f8172.rom

# Three processes: save; restore with the image present; restore with it
# deleted (see run.sh).
TEST_RUNNER := run.sh

TEST_SETUP := cp "$(TEST_DATA)/systems/System_6_0_8.dsk" "$(TEST_RESULTS_DIR)/vol.dsk"

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
