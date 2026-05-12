# Integration test configuration: IIcx Video Modes
# Cold-boots the IIcx four times with the System 7.0.1 floppy at
# 13" RGB (sense $6) — once per depth (1, 2, 4, 8 bpp) — with PRAM
# pre-seeded so the Slot Manager's GET_SLOT_DEPTH picks up the
# desired sResource at boot time.  Then cold-boots once per
# supported monitor sense (12" RGB / 15" Portrait B&W / 21" RGB
# Kong) without boot media and verifies the boot-icon screen at
# each resolution at 1bpp.  See test.script for the detailed setup
# and docs/pram.md §3/§6 for the validator + sPRAMRec layout.

TEST_NAME := IIcx Video Modes
TEST_DESC := Cold-boot Finder-at-N-bpp via PRAM seeding + resolution sweep on the JMFB.

TEST_ROM := roms/IIcx.rom

# 8 MB RAM lets the JMFB driver complete PrimaryInit cleanly and
# matches the iicx-floppy budget so the Finder boot timing transfers
# directly.
TEST_ARGS := model=iicx ram=8192 fd=$(TEST_DATA)/systems/System_7_0_1.image
