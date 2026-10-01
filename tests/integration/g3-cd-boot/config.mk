# Integration test: the Mac OS 9.2.1 install CD boots a beige G3 to the
# Finder twice, once from a SCSI CD-ROM on the MESH bus and once from an
# ATAPI CD-ROM on Heathrow's ATA cell 0.
#
# MEDIA-GATED.  The retail Mac OS 9.2.1 CD is not redistributable; it comes
# from $GS_EXTRA_MEDIA_DIR/macos-9.2.1/macos921.iso, and the row SKIPS
# cleanly without it.

TEST_NAME := G3 Mac OS 9.2.1 CD boot
TEST_DESC := Boots the Mac OS 9.2.1 CD to the Finder on a beige G3 from a SCSI CD-ROM and from an ATAPI CD-ROM

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

TEST_ARGS := model=pmg3dt ram=65536

# Two boots of ~13 G instructions each.
TEST_TIMEOUT := 3600

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
