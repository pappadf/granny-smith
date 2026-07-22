# Integration test configuration: SE/30 HD Floppy Boot
# Verifies that the SE/30 boots from a 1.44MB HD MFM floppy disk (System 7.0.1)

TEST_NAME := SE/30 HD Floppy Boot
TEST_DESC := Boots SE/30 with Universal ROM from 1.44MB HD floppy and verifies Finder desktop

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/iix-iicx-se30-97221136.rom

# Boot from 1.44MB HD MFM floppy (System 7.0.1)
# Pinned to the REAL onboard-video vROM kind: this suite's reference PNGs
# were captured against it, and the SE/30 profile now defaults to the
# generic GS-vROM sibling (proposal-generic-nubus-vrom.md stage 3).
TEST_ARGS := video_card=builtin_se30_video ram=8192 fd=$(TEST_DATA)/systems/System_7_0_1.image
