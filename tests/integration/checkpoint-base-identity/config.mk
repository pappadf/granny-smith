# Integration test: a quick checkpoint records each image's source key and
# refuses to restore onto a base that is no longer the same bytes.
#
# A quick checkpoint keeps only the image's delta: its disk is the base plus
# that delta, so restoring it onto a replaced base would put the saved
# changes on top of a different disk.  The checkpoint stores the base's key
# (canonical path, size and time stamp -- source.h) and the restore compares
# it.  The two floppies here are the same size, so only the key tells them
# apart.

TEST_NAME := Checkpoint base identity
TEST_DESC := A quick checkpoint restores onto its own base in a new process, and refuses a base replaced since

TEST_ROM := roms/plus-v3-4d1f8172.rom

# Three processes: save; restore onto the same base; restore after the
# base is replaced (see run.sh).
TEST_RUNNER := run.sh

# a.dsk carries a fixed old time stamp, so the copy over it in run.sh is a
# different key even within the same second as the setup.
TEST_SETUP := rm -rf "$(TEST_RESULTS_DIR)/cp" && mkdir -p "$(TEST_RESULTS_DIR)/cp" && cp "$(TEST_DATA)/systems/System_2_0_1.dsk" "$(TEST_RESULTS_DIR)/a.dsk" && touch -d "2001-02-03 04:05:06" "$(TEST_RESULTS_DIR)/a.dsk" && cp "$(TEST_DATA)/systems/System_3_0_0.dsk" "$(TEST_RESULTS_DIR)/b.dsk"

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
