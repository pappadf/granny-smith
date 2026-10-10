# Integration suite: part of the AV family suite, run as its own test so
# no single test sets the CI shard floor.  test.script names the rows
# (av-floppy-boot) and includes ../suite-av/test.script, whose header describes
# them; goldens/ links to that directory's goldens.
#
#   make test-suite-av-floppy-boot
#   make test-suite-av-floppy-boot TEST_VARS="ROW=av-floppy-boot"     one row only
#   make test-suite-av-floppy-boot TEST_VARS="REGEN=1"      recapture goldens

TEST_NAME := AV suite: floppy boot row
TEST_DESC := Cold boot from the Mac OS 7.6 install floppy to the Installer

TEST_ROM := roms/q840av-q660av-5bf10fd1.rom
TEST_ARGS := model=q840av ram=16384

TEST_TIMEOUT := 1200

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
