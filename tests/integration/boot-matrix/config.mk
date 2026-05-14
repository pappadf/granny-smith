# Integration test configuration: Boot Matrix
# Sequentially cold-boots a Macintosh Plus with multiple System
# Software versions from floppy and verifies each reaches a stable
# boot state.  All iterations share the same Plus ROM; the script
# uses `machine.boot "plus" 4096` to re-instantiate the machine
# between iterations (no daemon restart) so the entire matrix runs
# inside a single headless invocation.

TEST_NAME := Boot Matrix
TEST_DESC := Cold-boot a Plus across multiple System Software versions in one daemon run.

# Macintosh Plus ROM (shared across the whole matrix)
TEST_ROM := roms/Plus_v3.rom

# Start the daemon with a Plus profile at 1 MB so the first
# `machine.boot` in test.script has a sensible baseline to re-instantiate
# from.  Each iteration in test.script then re-instantiates the Plus at
# the RAM size matching that test row (1 / 2 / 2.5 / 4 MB), exercising
# all four officially-supported real-hardware Plus RAM configurations
# in a single test run.
TEST_ARGS := model=plus ram=1024
