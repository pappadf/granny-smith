# Integration test configuration: Machine Abstraction Wiring (M1)
# Exercises system_create → boot → checkpoint → system_destroy → system_create restore cycle

TEST_NAME := Machine Lifecycle (M1)
TEST_DESC := Validates system_create/system_destroy lifecycle and checkpoint round-trip via machine_plus callbacks

# Required ROM image (path relative to tests/data/)
TEST_ROM := roms/Plus_v3.rom

# No extra disk images required; uses TEST_RUNNER for multi-step execution
TEST_RUNNER := run.sh
