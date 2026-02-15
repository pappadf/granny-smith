# Integration test configuration: Boot to desktop
# This file defines test-specific parameters for the boot integration test

# Test description (shown when running)
TEST_NAME := Boot to Desktop
TEST_DESC := Verifies System 6.0.8 boots to the Finder desktop

# Required disk images (paths relative to tests/data/)
TEST_ROM := roms/Plus_v3.rom

# Emulator arguments (fd= for floppy, hd= for hard disk, etc.)
# $(TEST_DATA) is expanded by the parent Makefile to the absolute path
TEST_ARGS := fd=$(TEST_DATA)/systems/System_6_0_8.dsk
