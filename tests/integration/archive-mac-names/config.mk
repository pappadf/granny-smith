# Integration test: the MacRoman member names of a Mac archive (a StuffIt 5
# archive built at setup, its names holding the bytes 0x8E "é" and 0x8F "è")
# list as UTF-8 through files.list, are reachable by that name, matched
# without case as the Finder matches them, extract to the host and import
# under it, and a floppy read from such a member survives a checkpoint.  A
# zip's UTF-8 name is shown and matched as stored.

TEST_NAME := storage.archive-mac-names
TEST_DESC := A StuffIt archive's MacRoman names list and resolve as UTF-8; a zip's UTF-8 names are left as stored

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_SETUP := rm -rf "$(WORK_DIR)/out" "$(WORK_DIR)/system.dmg" "$(WORK_DIR)/wrong.dmg" && python3 archive-mac-names/make_fixtures.py "$(TEST_DATA)/systems/System_6_0_8.dsk" "$(WORK_DIR)"

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
