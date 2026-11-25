# Integration suite: the MEMCjr boards (LC 475, LC 575, Quadra 605)
#
# One daemon run; rows re-instantiate via machine.boot (the suite-djmemc
# shape) and ../lib/mac.lua provides the harness (row filter, keep-going,
# REGEN, condition-based waits).
#
# Every row boots one model to the Finder, asserts machine id, ROM id and
# the BoxFlag the ROM derived from the board's CPU-ID register, and
# pixel-matches the desktop and the "About This Macintosh" window (which
# names the model under System 7.1 + System Enabler 065).
#
#   make integration-test-suite-memcjr
#   make integration-test-suite-memcjr TEST_VARS="ROW=q605-71-hd"   one row only
#   make integration-test-suite-memcjr TEST_VARS="KEEP_GOING=1"     nightly mode
#   make integration-test-suite-memcjr TEST_VARS="REGEN=1"          recapture goldens

TEST_NAME := MEMCjr suite (LC 475, LC 575, Quadra 605)
TEST_DESC := 7.1 (Enabler 065), 7.5, 7.5.3 and 7.6 boots from SCSI and floppy to the About box on all three models

TEST_ROM := roms/lc475-lc575-q605-ff7439ee.rom
TEST_ARGS := model=q605 ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
