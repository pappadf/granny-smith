# Integration suite: the Power Macintosh 7200 ("Catalyst")
#
# One daemon run; rows re-instantiate via machine.boot (the suite-tnt
# shape) and ../lib/mac.lua provides the harness (row filter, keep-going,
# REGEN, condition-based waits).  The 7200 runs the 7500/8500/9500 ROM; each
# row names the revision it boots.
#
#   make integration-test-suite-pm7200
#   make integration-test-suite-pm7200 TEST_VARS="ROW=pm7200-76-hd"   one row only
#   make integration-test-suite-pm7200 TEST_VARS="KEEP_GOING=1"       nightly mode
#   make integration-test-suite-pm7200 TEST_VARS="REGEN=1"            recapture goldens

TEST_NAME := Power Macintosh 7200 suite
TEST_DESC := System 7.5.3, 7.6 and Mac OS 8.1 boot from the 53C94 disk to the Finder + About box on the Platinum board, and the System 7.6 "Install 1" floppy boots to the Installer

# 4 MB Power Macintosh 7500/8500/9500 ROM, v1 (stored checksum 0x96CD923D).
TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

TEST_ARGS := model=pm7200 ram=16384

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
