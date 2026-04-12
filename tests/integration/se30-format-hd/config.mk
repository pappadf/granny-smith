# Integration test configuration: SE/30 Format Blank HD
# Creates a blank 80 MB SCSI hard disk image and verifies HD SC Setup
# recognizes it under System 7.0.1

TEST_NAME := SE/30 Format Blank HD
TEST_DESC := Creates blank 80MB image via hd create, boots 7.0.1 floppy, opens HD SC Setup

# SE/30 Universal ROM
TEST_ROM := roms/SE30.rom

# Boot from floppy with 8 MB RAM; HD is created and attached in test.script
TEST_ARGS := fd=$(TEST_DATA)/systems/System_7_0_1.image ram=8192
