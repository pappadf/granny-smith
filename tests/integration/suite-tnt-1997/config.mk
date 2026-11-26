# Integration suite: the 1997 TNT machines (Power Macintosh 7300, 7600,
# 8600 and 9600)
#
# One daemon run; rows re-instantiate via machine.boot (the suite-tnt
# shape) and ../lib/mac.lua provides the harness (row filter, keep-going,
# REGEN, condition-based waits).  Each row names its ROM explicitly: the
# 1997 v1 ROM ($960E4BE9, all four models), the later 8600/9600 ROM
# ($960FC647), and the 1995 v2 ROM the first 7600s shipped with.
#
# Each writable attach mints its own delta instance under the test's
# storage cache, so no row sees another row's writes.
#
#   make integration-test-suite-tnt-1997
#   make integration-test-suite-tnt-1997 TEST_VARS="ROW=pm8600-81-hd"   one row only
#   make integration-test-suite-tnt-1997 TEST_VARS="KEEP_GOING=1"       nightly mode
#   make integration-test-suite-tnt-1997 TEST_VARS="REGEN=1"            recapture goldens

TEST_NAME := TNT 1997 suite (7300/7600/8600/9600)
TEST_DESC := Mac OS 8.1 boots to the Finder + About This Computer on all four 1997 TNT models (7300/7600 on the v1 ROM, 8600 on the later ROM, 9600 on both); the 7600 also boots System 7.6 from disk and floppy under the 1995 ROM

# 4 MB Power Macintosh 7300/7600/8600/9600 ROM (stored checksum 0x960E4BE9).
TEST_ROM := roms/pm7300-pm7600-pm8600-pm9600-960e4be9-949c2c56d07516b7.rom

TEST_ARGS := model=pm7600 ram=32768

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
