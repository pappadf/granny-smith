# Integration test configuration: IIfx Boot to "?" Disk
# Verifies that the IIfx ROM completes POST + ADB device-table scan and
# reaches the "insert disk" prompt screen with no boot media attached.

TEST_NAME := IIfx Boot to ? Disk
TEST_DESC := Boots Macintosh IIfx through POST + ADB init to the "insert disk" prompt screen

TEST_ROM := roms/4147DD77-IIfx.rom

# NOTE: ram=8192 currently regresses the boot (stalls in POST at $40843F96
# while probing the SCC IOP).  ram=16384 boots cleanly to the "?" disk.
# The 8 MB stall is tracked separately.
TEST_ARGS := model=iifx ram=16384
