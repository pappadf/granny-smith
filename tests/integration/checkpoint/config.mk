# Integration test configuration: Checkpoint Save/Load
# This test verifies checkpoint save and restore functionality

# Test description (shown when running)
TEST_NAME := Checkpoint Save/Load
TEST_DESC := Tests save-state and load-state across different boot configurations

# Required disk images (paths relative to tests/data/)
TEST_ROM := roms/Plus_v3.rom

# This test uses a custom runner script instead of the standard single-invocation pattern
# The runner script handles the two-step checkpoint test
TEST_RUNNER := run.sh

# Setup: unzip SCSI HD image to temp directory for step 2
TEST_SETUP := unzip -o -q $(TEST_DATA)/systems/hd1.zip -d $(TEST_TMPDIR)

# Note: TEST_ARGS not used when TEST_RUNNER is set
