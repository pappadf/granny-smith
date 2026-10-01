# Integration test: a ROM image loads from inside a zip archive.
#
# ROM loading opens its path through the VFS like any other consumer, so a
# ROM need not be unpacked first: setup deflates the Plus ROM into roms.zip
# (Python's zipfile, an independent writer) beside a text member, and the
# script boots from the ROM member and cats the text one.  Members are read
# through the zip namespace and decoded as they are read; nothing is
# extracted beside the archive.

TEST_NAME := storage.rom_from_archive
TEST_DESC := Boot a Plus from a ROM image read out of a zip archive

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_SETUP := python3 -c "import zipfile, sys; z = zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED); z.write(sys.argv[2], 'plus.rom'); z.writestr('README.txt', 'Macintosh Plus ROM, revision 3\n'); z.close()" "$(WORK_DIR)/roms.zip" "$(TEST_DATA)/roms/plus-v3-4d1f8172.rom"

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
