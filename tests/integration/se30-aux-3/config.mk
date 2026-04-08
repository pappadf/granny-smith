# Integration test configuration: SE/30 A/UX 3.0.1 Boot
# Verifies that the SE/30 boots from the A/UX 3.0.1 Installation Boot Disk

TEST_NAME := SE/30 A/UX 3.0.1 Boot
TEST_DESC := Boots SE/30 with Universal ROM from A/UX 3.0.1 Installation Boot Disk

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/SE30.rom

# 16 MB RAM required for A/UX boot, boot from A/UX 3.0.1 Installation Boot Disk
TEST_ARGS := ram=16384 fd=$(TEST_DATA)/aux/aux_3.0.1/AUX_Installation_Boot_Disk.img
