# Integration test configuration: Debug tooling
# Exercises new debug shell commands from proposal-debug-tooling.md.

TEST_NAME := Debug Tooling
TEST_DESC := Tests debug shell commands (find str/bytes) from PR1 of debug-tooling proposal

# Plus ROM — small, deterministic, always available.
TEST_ROM := roms/Plus_v3.rom

# Custom runner: we capture the emulator's stdout and grep for expected
# lines so the test verifies actual behavior, not just "didn't crash".
TEST_RUNNER := run.sh
