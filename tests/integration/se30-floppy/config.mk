# Integration test configuration: SE/30 Floppy Boot
# Verifies that the SE/30 boots from a System 7.1 floppy disk

TEST_NAME := SE/30 Floppy Boot
TEST_DESC := Boots SE/30 with Universal ROM from System 7.1 floppy and verifies display

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/IIcx.rom

# Boot from System 7.1 floppy disk
TEST_ARGS := fd=$(TEST_DATA)/systems/System_7_1_0.dsk
