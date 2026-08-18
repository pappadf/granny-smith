# Integration test: PDM ROM boot ladder (proposal-powerpc-601-pdm.md §6.1)
#
# THE verification instrument for the PowerPC/PDM bring-up: boots the
# shipping 1994-03 PDM ROM headless for a bounded instruction budget and
# asserts every ladder marker up to the current high-water rung.  The
# expected rung is committed alongside the code that reaches it; any change
# that drops a rung fails this row with the name of the first missing
# marker — not a vague "boot broke".
#
# Current high-water: L20 (boot chime golden-WAV-exact through the AWACS
# datapath, gray-desktop screen golden through the Ariel scanout); the run
# parks at the Phase-G SCSI-scan wall.  REGEN=1 regenerates both goldens.

TEST_NAME := PDM ROM ladder
TEST_DESC := Boots the PDM ROM and asserts the §6.1 ladder markers up to the committed high-water rung

# 4 MB Power Macintosh 6100/7100/8100 ROM (stored checksum 0x9FEB69B3)
TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom

# 24 MB exercises the 6100 SIMM_BANK_SIZE relocation (8 MB soldered +
# two 8 MB banks packed contiguously by the config write).
TEST_ARGS := model=pm6100 ram=24576

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
