# Integration test: Gossamer ROM boot ladder
#
# THE verification instrument for the beige Power Macintosh G3 bring-up:
# boots the Rev C ROM headless for a bounded instruction budget and asserts
# every ladder marker up to the current high-water rung, then boots the
# Rev A ROM and asserts the rungs the two share.  Any change that drops a
# rung fails this row with the name of the first missing marker.
#
# Current high-water: G8 (68k dispatching on the 750, the 60.15 Hz tick
# chain at rate, the Cuda clock advancing Time), plus G10 (the boot beep,
# sample-exact against a golden WAV).  G6's full property-by-
# property comparison against the real machine is gossamer-device-tree.

TEST_NAME := Gossamer ROM ladder
TEST_DESC := Boots the beige G3 ROMs (Rev C, Rev A) and asserts the ladder markers up to the committed high-water rung

# 4 MB Power Macintosh G3 ROM Rev C ($077D.45F2, Open Firmware 2.4).
TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

TEST_ARGS := model=pmg3dt ram=65536

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
