# Integration test configuration: SE/30 MacTest Boot
# Verifies that the SE/30 boots MacTest SE/30 from floppy and reaches the main UI

TEST_NAME := SE/30 MacTest Boot
TEST_DESC := Boots SE/30 with MacTest SE/30 floppy, dismisses splash, verifies test UI

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/SE30.rom

# MacTest SE/30 floppy disk image (800K DiskCopy)
TEST_ARGS := fd=$(TEST_DATA)/apps/MacTest-SE30.image
