# Integration test: the mouse guard and the mouse trace belong to the machine.
#
# Both are scheduler events whose source is the machine's input object, so a
# new machine starts with neither, `mouse.trace true` works again after a
# machine.boot, and a checkpoint taken with either running restores it.

TEST_NAME := Mouse guard and trace are per machine
TEST_DESC := mouse.trace works after machine.boot; a checkpoint with the MTemp guard and the trace running restores both

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
