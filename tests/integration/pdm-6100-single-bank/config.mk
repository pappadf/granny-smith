# Integration test: Power Macintosh 6100 with one SIMM bank populated
#
# A single-sided SIMM pair is one bank, leaving SIMM bank 2 empty: the
# ROM then keeps SIMM_BANK_SIZE at its reset code 0 and records bank 1 at
# the motherboard top.  Boots 10, 16 (the profile default) and 40 MB to
# the gray desktop and asserts the bank table the ROM hands the kernel.
# REGEN=1 regenerates the screen golden.

TEST_NAME := PDM 6100 single SIMM bank
TEST_DESC := Boots the 6100 with SIMM bank 2 empty (10/16/40 MB) to the gray desktop

# 4 MB Power Macintosh 6100/7100/8100 ROM (stored checksum 0x9FEB69B3)
TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom

TEST_ARGS := model=pm6100 ram=16384

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
