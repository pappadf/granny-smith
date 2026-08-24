# Integration test: TNT ROM boot ladder (proposal-powermac-7500-8500-9500 §7.1)
#
# THE verification instrument for the TNT bring-up: boots the shipping
# 1995-08 TNT ROM headless for a bounded instruction budget and asserts
# every ladder marker up to the current high-water rung.  The expected
# rung is committed alongside the code that reaches it; any change that
# drops a rung fails this row with the name of the first missing marker.
#
# Current high-water: T9 (68k dispatching, low memory live, the 60.15 Hz
# tick chain at rate, the DBDMA engine executing the ROM's own beep
# program, the interrupt fabric's mode-1 discipline visible in the
# registers); the run parks at the Phase-D video wall (no Control model
# yet — ScrnBase stays 0).

TEST_NAME := TNT ROM ladder
TEST_DESC := Boots the TNT ROM and asserts the §7.1 ladder markers up to the committed high-water rung

# 4 MB Power Macintosh 7500/8500/9500 ROM v1 (stored checksum 0x96CD923D)
TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

TEST_ARGS := model=pm7500 ram=32768

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
