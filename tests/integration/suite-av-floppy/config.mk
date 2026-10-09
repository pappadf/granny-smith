# Integration suite: part of the AV family suite, run as its own test so
# no single test sets the CI shard floor.  test.script names the rows
# (av-floppy-mount, av-floppy-800k, av-floppy-write-eject, av-floppy-format) and includes ../suite-av/test.script, whose header describes
# them; goldens/ links to that directory's goldens.
#
#   make test-suite-av-floppy
#   make test-suite-av-floppy TEST_VARS="ROW=av-floppy-mount"     one row only
#   make test-suite-av-floppy TEST_VARS="REGEN=1"      recapture goldens

TEST_NAME := AV suite: floppy rows
TEST_DESC := New Age FDC: 1.44 MB and 800K mounts, write and eject, Finder format

TEST_ROM := roms/q840av-q660av-5bf10fd1.rom
TEST_ARGS := model=q840av ram=16384

TEST_TIMEOUT := 1200

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
