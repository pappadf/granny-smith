# Integration test: Mac OS 9.2.1 installed from its CD onto a blank ATA disk
# on a beige G3, the CD itself on an ATAPI drive.
#
# MEDIA-GATED.  The retail Mac OS 9.2.1 CD comes from
# $GS_EXTRA_MEDIA_DIR/macos-9.2.1/macos921.iso; the row SKIPS without it.

TEST_NAME := G3 Mac OS 9.2.1 ATA install
TEST_DESC := Boots the Mac OS 9.2.1 CD from an ATAPI drive on a beige G3, initializes a blank ATA disk with Drive Setup and installs Mac OS 9.2.1 onto it

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

TEST_ARGS := model=pmg3dt ram=65536

# ~70 G instructions: CD boot, Drive Setup, the install.
TEST_TIMEOUT := 7200

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
