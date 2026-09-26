# Integration test: the image VFS refuses a file the emulator holds writable.
#
# A writable attach puts the guest's writes in a delta the VFS's read-only
# mount cannot see, so the VFS must answer EBUSY rather than serve the stale
# base.  The unit suite image_vfs covers the mount side against a stubbed
# query; this covers the real one end to end: the attach paths register the
# image, and the key is the path the user named even when the image actually
# opened is a decoded scratch copy (the .dmg).
#
# The .dmg is built at setup time by image-udif's fixture writer.

TEST_NAME := storage.attach-busy
TEST_DESC := The image VFS refuses hard disks and floppies attached writable, and only those

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_SETUP := python3 image-udif/make-fixture.py "$(TEST_DATA)/systems/system_6_0_8_20mb_8_24gc.img" "$(WORK_DIR)/fixture.dmg" && cp "$(TEST_DATA)/systems/System_6_0_8.dsk" "$(WORK_DIR)/floppy.dsk" && cp "$(TEST_DATA)/systems/system_6_0_8_20mb_8_24gc.img" "$(WORK_DIR)/hd.img"

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
