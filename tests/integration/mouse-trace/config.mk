# Integration test: the mouse trace belongs to the machine.
#
# It is a scheduler event whose source is the machine's input object, so a
# new machine starts without it, `mouse.trace true` works again after a
# machine.boot, and a checkpoint taken with it running restores it.

TEST_NAME := Mouse trace is per machine
TEST_DESC := mouse.trace works after machine.boot; a checkpoint with the trace running restores it

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
