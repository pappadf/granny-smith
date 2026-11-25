# Integration suite: the djMEMC family (Centris 610/650, Quadra 610/650/800)
#
# One daemon run; rows re-instantiate via machine.boot (the suite-quadra
# shape) and ../lib/mac.lua provides the harness (row filter, keep-going,
# REGEN, condition-based waits).
#
# Every row boots one model to the Finder, asserts machine id, ROM id and
# the BoxFlag the ROM derived from the board's identity straps, and
# pixel-matches the desktop and the "About This Macintosh" window (which
# names the model -- the Gestalt identity end-to-end).
#
#   make integration-test-suite-djmemc
#   make integration-test-suite-djmemc TEST_VARS="ROW=q800-75-hd"   one row only
#   make integration-test-suite-djmemc TEST_VARS="KEEP_GOING=1"     nightly mode
#   make integration-test-suite-djmemc TEST_VARS="REGEN=1"          recapture goldens

TEST_NAME := djMEMC suite (Centris 610/650, Quadra 610/650/800)
TEST_DESC := 7.1 (Enabler 040), 7.5, 7.5.3 and 7.6 boots from SCSI and floppy to the About box on all five models

TEST_ROM := roms/c610-c650-q610-q650-q800-f1acad13.rom
TEST_ARGS := model=q800 ram=16384

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
