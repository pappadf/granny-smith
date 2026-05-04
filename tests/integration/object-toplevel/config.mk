# Integration test: M8 top-level objects (appletalk, mouse)
# First M8 slice — exercises appletalk.shares + .printer and
# mouse.{move,click,trace}. Storage and root introspection methods
# land in a follow-up M8 sub-commit.

TEST_NAME := Object-model M8 top-level (appletalk + mouse)
TEST_DESC := appletalk.shares/.printer + mouse.* methods

TEST_ROM := roms/Plus_v3.rom
