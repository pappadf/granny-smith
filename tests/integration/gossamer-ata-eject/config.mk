# Integration test: the host takes an ATA hard disk off the beige G3's bus
# the way it takes one off a SCSI bus.  eject_media on an ATA hard disk was
# refused ("not removable media") while the same verb on a SCSI hard disk
# detached it; now both detach, and the image goes back to the machine's
# image list, which closes it.

TEST_NAME := storage.ata_hd_eject
TEST_DESC := eject_media takes an ATA hard disk off the bus and releases its image

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom
TEST_SETUP := truncate -s 20M "$(WORK_DIR)/ata.img"
TEST_ARGS := model=pmg3dt

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
