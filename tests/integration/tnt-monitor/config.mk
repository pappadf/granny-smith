# Integration test: the TNT built-in port takes a monitor= pick (#146).
#
# Control's monitor sense strap was a compile-time constant (the 13"/14"
# strap), so the ROM only ever selected 640x480 on the 7500/8500.  The
# strap now comes from the boot document's monitor=, the way the PDM's
# does, and Open Firmware programs the timing set for the monitor it reads.

TEST_NAME := TNT built-in monitor pick
TEST_DESC := pm7500 monitor=twopage boots Control to 1152x870, keeps it across machine.restart; portrait gives 640x870

TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

TEST_ARGS := model=pm7500 ram=32768

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
