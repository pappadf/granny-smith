# Integration test: a consolidated checkpoint of a machine whose floppy is a
# member of a zip archive loads without a false error.
#
# A consolidated restore recreates a base image only when it is missing.  It
# used to ask the host (access()), which has no file inside an archive, so
# loading such a checkpoint tried to create the floppy inside the zip and
# printed "cannot recreate the missing base ..." over a restore that then
# succeeded (S1018-32).  The archive is made at setup with Python's zipfile.

TEST_NAME := Checkpoint archive base
TEST_DESC := A checkpoint with a floppy read from a zip archive loads with no missing-base error

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_RUNNER := run.sh

TEST_SETUP := rm -rf "$(TEST_RESULTS_DIR)/cp" && mkdir -p "$(TEST_RESULTS_DIR)/cp" && cp "$(TEST_DATA)/systems/System_6_0_8.dsk" "$(TEST_RESULTS_DIR)/sys608.dsk" && python3 -c "import zipfile, sys; z = zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED); z.write(sys.argv[2], 'sys608.dsk'); z.close()" "$(TEST_RESULTS_DIR)/floppies.zip" "$(TEST_DATA)/systems/System_6_0_8.dsk"

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
