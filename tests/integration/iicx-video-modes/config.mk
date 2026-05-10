# Integration test configuration: IIcx Video Modes
# Boots IIcx with the System 7.0.1 floppy, drives the Apple Macintosh
# Display Card 8•24 (JMFB) through 1, 2, 4, and 8 bpp at 640x480 by
# calling SetDepth on the active GDevice from the running OS, and
# verifies the rendered Finder desktop at each depth.  Then cold-
# boots once per supported monitor sense (12" RGB / 15" Portrait
# B&W / 21" RGB Kong) and verifies the boot-icon screen at each
# resolution at 1bpp.  See tests/integration/iicx-video-modes/test.script
# for the detailed setup, and docs/pram.md §9 for the reasoning.

TEST_NAME := IIcx Video Modes
TEST_DESC := Real Finder-at-N-bpp via SetDepth + cold-boot resolution sweep on the JMFB.

TEST_ROM := roms/IIcx.rom

# 8 MB RAM lets the JMFB driver complete PrimaryInit cleanly and
# matches the iicx-floppy budget so the Finder boot timing transfers
# directly.
TEST_ARGS := model=iicx ram=8192 fd=$(TEST_DATA)/systems/System_7_0_1.image
