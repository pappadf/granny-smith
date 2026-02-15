# Integration test configuration: Checkpoint Save/Load v2
# This test verifies checkpoint save and restore functionality
# Unlike checkpoint, this test uses static scripts and persistent test-results directory

# Test description (shown when running)
TEST_NAME := Checkpoint Save/Load v2
TEST_DESC := Tests save-state and load-state with persistent test artifacts

# Required disk images (paths relative to tests/data/)
TEST_ROM := roms/Plus_v3.rom

# This test uses a custom runner script that operates on static script files
# and uses a persistent test-results directory instead of /tmp
TEST_RUNNER := run.sh

# Setup: unzip SCSI HD image and copy floppy image to test-results directory.
# Floppy must be copied so that consolidated checkpoint restore (which
# recreates backing files in-place) does not overwrite the original test data.
# TEST_RESULTS_DIR is set by the parent Makefile
TEST_SETUP := unzip -o -q $(TEST_DATA)/systems/hd1.zip -d $(TEST_RESULTS_DIR) && cp $(TEST_DATA)/systems/System_6_0_8.dsk $(TEST_RESULTS_DIR)/System_6_0_8.dsk

# Note: TEST_ARGS not used when TEST_RUNNER is set
