# Integration test configuration: Debug tooling
# Exercises the debug shell commands (find, breakpoints, ...).

TEST_NAME := Debug Tooling
TEST_DESC := Tests debug shell commands (find str/bytes)

# Plus ROM — small, deterministic, always available.
TEST_ROM := roms/plus-v3-4d1f8172.rom

# Custom runner: we capture the emulator's stdout and grep for expected
# lines so the test verifies actual behavior, not just "didn't crash".
TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
