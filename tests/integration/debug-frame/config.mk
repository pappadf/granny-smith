# Integration test: debug.frame on every CPU architecture,
# and the same frame on machine.cpu and the AV DSP3210.
# The web Debug view renders from debug.frame; it used to read the 68K
# cpu_t and failed on every PowerPC machine.  This row boots one machine per
# CPU family and checks the frame's contract on each.

TEST_NAME := debug.frame on every CPU family
TEST_DESC := arch-tagged register file, rows anchored on the PC, on 68000/Lisa/030/040/601/604 and the DSP3210

TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
