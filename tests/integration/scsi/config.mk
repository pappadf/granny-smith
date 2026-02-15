# Integration test configuration: SCSI Hard Disk Boot
# This file defines test-specific parameters for the SCSI integration test

# Test description (shown when running)
TEST_NAME := SCSI Hard Disk Boot
TEST_DESC := Verifies booting from a SCSI hard disk image (hd1.zip)

# Required disk images (paths relative to tests/data/)
TEST_ROM := roms/Plus_v3.rom

# Setup command: unzip hd1.zip to a temp directory before running
# $(TEST_DATA) and $(TEST_TMPDIR) are expanded by the parent Makefile
TEST_SETUP := unzip -o -q $(TEST_DATA)/systems/hd1.zip -d $(TEST_TMPDIR)

# Emulator arguments (hd= for hard disk)
# $(TEST_TMPDIR) is the temporary directory created for this test run
TEST_ARGS := hd=$(TEST_TMPDIR)/hd1.img
