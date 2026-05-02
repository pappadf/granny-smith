# Integration test: M8 top-level objects (network, input)
# First M8 slice — exercises network.appletalk.shares + .printer and
# input.mouse.{move,click,trace}. Storage and root introspection
# methods land in a follow-up M8 sub-commit.

TEST_NAME := Object-model M8 top-level (network + input)
TEST_DESC := network.appletalk.shares/.printer + input.mouse.* methods

TEST_ROM := roms/Plus_v3.rom
