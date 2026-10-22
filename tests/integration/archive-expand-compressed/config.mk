# Integration test: a disk image stored compressed inside an archive is
# expandable in files.list (the Filesystem tree and the archive upload
# route on that flag): a deflated zip member and a disk in a StuffIt
# archive inside BinHex, as a stored zip member already was.  A compressed
# member over the 64 MB a listing decodes is not probed.
#
# The zips are built at setup time (make_fixtures.py); the .sit.hqx is the
# MacTest disk from the test data.

TEST_NAME := storage.archive-expand-compressed
TEST_DESC := files.list marks a disk inside a deflated zip member or a .sit.hqx expandable, up to 64 MB unpacked

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_SETUP := python3 archive-expand-compressed/make_fixtures.py "$(TEST_DATA)/systems/System_6_0_8.dsk" "$(WORK_DIR)" && cp "$(TEST_DATA)/apps/MacTest_Disk.image_.sit_.hqx" "$(WORK_DIR)/mactest.sit.hqx"

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
