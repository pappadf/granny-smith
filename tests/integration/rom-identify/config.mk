# Integration test: rom.identify / machine.profile probe surface.
#
# Drives the cross-check that prevents drift between the C-side ROM_TABLE
# and what the frontend's machine-config dialog expects.  Plus is enough as
# the boot ROM; the test also probes the SE/30 ROM via path to assert the
# Universal-ROM compatibility list (se30 / iicx / iix).

TEST_NAME := ROM identify and machine profile probes
TEST_DESC := rom.identify map shape, machine.profile static lookup, machine.boot validation

TEST_ROM := roms/Plus_v3.rom
