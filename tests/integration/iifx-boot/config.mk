# Integration test configuration: IIfx Floppy Boot to Finder
# Verifies that the Macintosh IIfx ROM boots from a 1.44MB HD MFM floppy
# (System 7.0.1) through full POST + ADB device-table scan + .Sony driver
# init + SWIM IOP slot-2 floppy I/O + boot-block + System loader and
# reaches the Finder desktop with the "Disk Tools" volume mounted.

TEST_NAME := IIfx Floppy Boot to Finder
TEST_DESC := Boots Macintosh IIfx from System 7.0.1 floppy and verifies the Finder desktop

TEST_ROM := roms/4147DD77-IIfx.rom

# NOTE: ram=8192 currently regresses the boot (stalls in POST at $40843F96
# while probing the SCC IOP).  ram=16384 boots cleanly through to Finder.
# The 8 MB stall is tracked separately.
TEST_ARGS := model=iifx ram=16384 fd=$(TEST_DATA)/systems/System_7_0_1.image
