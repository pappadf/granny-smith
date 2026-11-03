# Integration test: Apple's MacTest Pro (PPC) CD v11.0 (December 1998) on a
# Power Macintosh 8500 -- the disc in the 8500's own CD-ROM drive boots the
# machine to its Mac OS 8.1 Finder, MacTest Pro for Power Macintosh 1.0.9
# identifies Gestalt machine 69, and all seven of its tests pass against the
# emulated hardware (the floppy test on a blank 1.44 MB disk).
#
# MEDIA: tests/data/apps/mactest_pro_ppc_dec_1998.iso (gs-test-data, stored
# as .7z; the whole disc, Apple partition map and CD driver included).

TEST_NAME := PM 8500 MacTest Pro (PPC)
TEST_DESC := Boots the MacTest Pro (PPC) Dec 1998 CD on a Power Macintosh 8500 and passes all seven of Apple's hardware tests

TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

# The script boots the machine itself, to insert the CD after construction.
TEST_ARGS := model=pm8500 ram=32768

# ~40 G instructions: boot, the app's load, the seven tests.
TEST_TIMEOUT := 3600

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
