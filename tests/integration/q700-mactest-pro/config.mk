# Integration test: Apple's MacTest Pro (680x0) CD (February 1996) on a
# Macintosh Quadra 700 -- the disc boots the machine from a SCSI CD-ROM to its
# own System 7.5 Finder, MacTest Pro identifies a "Macintosh Quadra 700", and
# all seven of its tests pass against the emulated hardware (the floppy test
# on a blank 1.44 MB disk).
#
# MEDIA: tests/data/apps/mactest_pro_68k_feb_1996.iso (gs-test-data, stored
# as .7z; the whole disc, Apple partition map and CD driver included).

TEST_NAME := Quadra 700 MacTest Pro (68k)
TEST_DESC := Boots the MacTest Pro (680x0) Feb 1996 CD on a Quadra 700 and passes all seven of Apple's hardware tests

TEST_ROM := roms/q700-q900-420dbff3.rom

# The script boots the machine itself, to attach the CD after construction.
TEST_ARGS := model=q700

# ~10 G instructions: boot, the app's load, the seven tests.
TEST_TIMEOUT := 1800

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
