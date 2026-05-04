# Integration test: debug.* indexed-child surface (M6)
# Boots Plus, exercises the debug.breakpoints / .logpoints tree, and
# verifies legacy `break` commands and the new `debug.breakpoints.add`
# method touch the same entries.

TEST_NAME := Object-model debug consolidation
TEST_DESC := debug.breakpoints/.logpoints indexed children with sparse stable indices

TEST_ROM := roms/Plus_v3.rom
