# Integration test: debug.frame on every CPU architecture (11-WORK-ORDER D2).
# The web Debug view renders from debug.frame; it used to read the 68K
# cpu_t and failed on every PowerPC machine.  This row boots one machine per
# CPU family and checks the frame's contract on each.

TEST_NAME := debug.frame on every CPU family
TEST_DESC := arch-tagged register file, rows anchored on the PC, on 68000/Lisa/030/040/601/604

TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
