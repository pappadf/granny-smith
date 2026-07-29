# Integration suite: the Quadra family (Q700 / Q900 / Q950)
# (proposal-integration-test-rework §7 — the §9 pilot suite)
#
# One daemon run; rows re-instantiate via machine.boot (the boot-matrix
# pattern) and the shared library in ../lib/mac.script provides the
# harness (row filter, keep-going, REGEN, condition-based waits).
# Absorbs the former q700-boot-chime, q700-boot-finder, q700-boot-hd,
# q900-boot-floppy, q900-boot-hd, and q950-boot-hd directories; their
# unique assertions live on as rows (see test.script's row map).
#
#   make test-suite-quadra
#   make test-suite-quadra TEST_VARS="ROW=q700-chime"     one row only
#   make test-suite-quadra TEST_VARS="KEEP_GOING=1"       nightly mode
#   make test-suite-quadra TEST_VARS="REGEN=1"            recapture goldens

TEST_NAME := Quadra suite (Q700/Q900/Q950)
TEST_DESC := Chime, floppy/HD boots, 7.1/7.5/7.6 spread, Thousands, checkpoint round-trip

TEST_ROM := roms/q700-q900-420dbff3.rom
TEST_ARGS := model=q700 ram=8192

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
