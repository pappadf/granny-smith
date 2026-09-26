# Integration suite: the Apple Network Server's screen rows (ANS 500)
#
# One daemon run; rows re-instantiate via machine.boot (the suite-pdm
# shape) and ../lib/mac.script provides the harness (row filter,
# keep-going, REGEN, condition-based waits).  Absorbs the former
# ans-console, ans-macos-2rom and ans-diag-floppy directories; their
# assertions live on as rows (see test.script's row map).
#
# TWO ROMS.  The Open Firmware rows use TEST_ROM below; the Mac OS row
# loads the 2.0 prototype ROM by path.
#
#   make test-suite-ans
#   make test-suite-ans TEST_VARS="ROW=ans500-of-console"   one row only
#   make test-suite-ans TEST_VARS="KEEP_GOING=1"            nightly mode
#   make test-suite-ans TEST_VARS="REGEN=1"                 recapture goldens

TEST_NAME := ANS suite (Network Server 500)
TEST_DESC := The 54M30 as Open Firmware's console, the 2.0 prototype ROM's Mac OS desktop on the same hardware model, and the Diagnostic Utility booted from the internal floppy

# 4 MB Apple Network Server 500/700 ROM, Open Firmware 1.1.22.  The Mac OS
# row loads roms/ans500-ans700-proto20-49b2be8f.rom itself.
TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=32768

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
