# Integration test: debugger.* indexed-child surface (M6)
# Boots Plus, exercises the new debugger.breakpoints / .logpoints
# tree, and verifies legacy `break` commands and the new
# `debugger.breakpoints.add` method touch the same entries.

TEST_NAME := Object-model debugger consolidation
TEST_DESC := debugger.breakpoints/.logpoints/.watches indexed children with sparse stable indices

TEST_ROM := roms/Plus_v3.rom
