# Integration test: ${...} interpolation in logpoint messages (M5)
# Boots Plus, sets a memory logpoint on Ticks ($16A), runs long enough
# for the VBL handler to bump it, and verifies the formatted output.

TEST_NAME := Object-model logpoint interpolation
TEST_DESC := ${...} interpolation drives logpoint message formatting

TEST_ROM := roms/Plus_v3.rom
