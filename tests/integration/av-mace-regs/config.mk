# Integration test: the AV MACE register file follows the Am79C940 map.
#
# The Quadra 840AV's MACE stub (src/machines/av/mace.c) used to number its
# registers off the data sheet: CHIPID at 24/25 (the missed-packet counter
# and a reserved slot), BIUCC/FIFOCC/PLSCC one or two slots high.  A driver
# reading the chip ID at registers 16/17 ($50F1C100/$50F1C110) got latched
# zero.  docs/reference/machines/av/mace.md §2.1 is the map; the TNT model
# (tnt_mace.c) already used it.

TEST_NAME := AV MACE register map
TEST_DESC := q840av MACE CHIPID at registers 16/17, MPC (24) reads zero, BIUCC/FIFOCC/PLSCC at 11/12/14

TEST_ROM := roms/q840av-q660av-5bf10fd1.rom
TEST_ARGS := model=q840av ram=16384

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
