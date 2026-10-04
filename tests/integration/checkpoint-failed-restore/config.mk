# Integration test: a restore that fails leaves the running machine exactly as
# it was, and a checkpoint saved with typed keys in flight restores.
#
# A restore builds the new machine while the running one stays active and
# untouched; only a complete build is swapped in.  For each family a run is
# interrupted by a checkpoint.load that fails late in construction (the file
# is cut short, so its last block -- the event queue -- is incomplete), and
# must end in exactly the state of the same run without the attempt: CPU
# registers, the scheduler's clock and the machine node's label.

TEST_NAME := A failed restore changes nothing
TEST_DESC := Failed checkpoint.load mid-run leaves each family's machine bit-identical to a control run; typed keys restore

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
