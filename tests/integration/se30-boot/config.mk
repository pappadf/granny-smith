# Integration test configuration: SE/30 Boot
# Verifies that the SE/30 boots to the floppy icon screen

TEST_NAME := SE/30 Boot
TEST_DESC := Boots SE/30 with Universal ROM to the floppy icon screen and verifies display

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/IIcx.rom

# No extra disk images; boot to floppy icon (no bootable disk)
TEST_ARGS :=
