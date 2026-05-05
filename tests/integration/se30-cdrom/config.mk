# Integration test configuration: SE/30 CD-ROM
# Verifies that the SE/30 boots with a CD-ROM device on the SCSI bus
# and that CD-ROM shell commands (attach/info/eject) work correctly.

TEST_NAME := SE/30 CD-ROM
TEST_DESC := Boots SE/30 with CD-ROM on SCSI bus, verifies Finder desktop and cdrom shell commands

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/SE30.rom

# Boot from 1.44MB HD MFM floppy (System 7.0.1) with CD-ROM on SCSI ID 3
TEST_ARGS := ram=8192 fd=$(TEST_DATA)/systems/System_7_0_1.image cdrom=$(TEST_DATA)/aux/aux_3.0.1/APPLE_AUX_3-0-1_RETAIL.iso
