# Integration test: the built-in video's startup mode.
#
# The boot document's displays.builtin.mode is the saved video mode the
# machine starts in: the seeding step writes the record the Monitors control
# panel would have saved into the built-in video's slot PRAM record, and the
# ROM brings the screen up at that depth.  One ROM-stage boot per family
# whose ROM keeps that record -- the IIci's RBV (slot $B), a Quadra 700's and
# a Quadra 950's DAFB (slot $9), and the 840AV's CIVIC (slot $9) -- each at a
# depth other than the one it starts at without the record.

TEST_NAME := Built-in video startup mode
TEST_DESC := displays.builtin.mode seeds the built-in video's slot PRAM record on the IIci, Quadra 700, Quadra 950 and 840AV

TEST_ROM := roms/iici-368cadfe.rom
TEST_ARGS := model=iici ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
