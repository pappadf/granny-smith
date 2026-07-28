# Integration test configuration: IIcx Video Modes
# Cold-boots the IIcx with the System 7.0.1 floppy at 13" RGB (sense $6)
# and 12" RGB (sense $2) — once per depth (1, 2, 4, 8 bpp) — with PRAM
# pre-seeded so the Slot Manager's GET_SLOT_DEPTH picks up the desired
# sResource at boot time.  See test.script for the detailed setup and
# docs/core/memory/pram.md §3/§6 for the validator + sPRAMRec layout.
#
# The 15" Portrait and 21" RGB halves of this sweep were re-hosted to
# suite-iix (§7's 8-of-16 split); the sixteen cells now span two hosts.

TEST_NAME := IIcx Video Modes
TEST_DESC := Cold-boot Finder-at-N-bpp via PRAM seeding on the JMFB (13" + 12" halves; 15"/21" run in suite-iix)

TEST_ROM := roms/iix-iicx-se30-97221136.rom

# 8 MB RAM lets the JMFB driver complete PrimaryInit cleanly and
# matches the iicx-floppy budget so the Finder boot timing transfers
# directly.
TEST_ARGS := model=iicx ram=8192 fd=$(TEST_DATA)/systems/System_7_0_1.image

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
