# Integration test configuration: Plus MacTest
# Boots Macintosh Plus with MacTest floppy disk and runs the hardware test suite

TEST_NAME := Plus MacTest
TEST_DESC := Boots Plus with MacTest floppy, deselects unsupported tests, runs suite, verifies pass

# Macintosh Plus ROM
TEST_ROM := roms/Plus_v3.rom

# MacTest floppy disk image (pre-extracted from MacTest_Disk.image_.sit_.hqx)
TEST_ARGS := fd=$(TEST_DATA)/apps/MacTest-Plus.image
