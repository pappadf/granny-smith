# Integration test: the reorganised object model (object-model proposal, phase 1).
#
# The root holds, in a fixed order, machine scheduler checkpoint files debug
# log shell catalog appletalk.  `files` merges the former storage / vfs /
# archive nodes and the root download method, with the image-VFS mount cache
# as files.mounts[serial]; `log` holds the log categories as a keyed
# collection; `catalog` holds what the emulator can build or fit;
# debug.find is per debug object and survives a checkpoint load; and
# scheduler.mode is a strict enum.

TEST_NAME := Object model organisation
TEST_DESC := root order, files.mounts serials, log.category, catalog, strict scheduler.mode, debug.find across a load

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_ARGS := model=plus

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
