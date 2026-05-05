# Integration test: floppy object class (M7e)
# Boots Plus with a System 6.0.8 floppy in fd0 so drive 0 is populated
# and drive 1 is empty — the test walks both slots to verify
# `present` / `disk` correctly reflect the load state.

TEST_NAME := Object-model floppy class
TEST_DESC := floppy.drives[0|1] indexed children + eject method

TEST_ROM := roms/Plus_v3.rom
TEST_ARGS := fd0=$(TEST_DATA)/systems/System_6_0_8.dsk
