# Integration test: Mac OS 9.2.1 installed from its CD onto a blank SCSI disk
# on a beige G3, and the installed disk booted to its desktop.
#
# MEDIA-GATED.  The retail Mac OS 9.2.1 CD comes from
# $GS_EXTRA_MEDIA_DIR/macos-9.2.1/macos921.iso; the row SKIPS without it.

TEST_NAME := G3 Mac OS 9.2.1 SCSI install
TEST_DESC := Initializes a blank SCSI disk with Drive Setup, installs Mac OS 9.2.1 from the CD on a beige G3, and boots the result

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

TEST_ARGS := model=pmg3dt ram=65536

# ~70 G instructions: CD boot, Drive Setup, the install, the HD boot.
TEST_TIMEOUT := 7200

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
