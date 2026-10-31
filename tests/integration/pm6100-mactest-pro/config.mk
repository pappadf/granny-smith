# Integration test: Apple's MacTest Pro (PPC) CD v11.0 (December 1998) on a
# Power Macintosh 6100 -- the disc boots the machine from a SCSI CD-ROM to its
# own Mac OS 8.1 Finder, MacTest Pro for Power Macintosh 1.0.9 identifies
# Gestalt machine 75, and all seven of its tests pass against the
# emulated hardware (the floppy test on a blank 1.44 MB disk).
#
# MEDIA: tests/data/apps/mactest_pro_ppc_dec_1998.iso (gs-test-data, stored
# as .7z; the whole disc, Apple partition map and CD driver included).

TEST_NAME := PM 6100 MacTest Pro (PPC)
TEST_DESC := Boots the MacTest Pro (PPC) Dec 1998 CD on a Power Macintosh 6100 and passes all seven of Apple's hardware tests

TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom

# The script boots the machine itself, to attach the CD after construction.
TEST_ARGS := model=pm6100 ram=24576

# ~25 G instructions: boot, the app's load, the seven tests.
TEST_TIMEOUT := 3600

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
