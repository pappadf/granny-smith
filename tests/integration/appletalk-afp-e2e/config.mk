# Integration test: AFP end-to-end fork fidelity
#
# The flagship of proposal-afp-server-completeness.md §7.3: one deterministic
# script that walks the whole chain — HFS image -> VFS copy-out -> AppleDouble
# pair on the host share -> AFP -> a guest that sees the right Finder Info,
# lists the volume, and *executes* a binary whose CODE resources live only in
# the resource fork — and then back out again with a byte-for-byte comparison.
#
# The launchable binary is built by make-fixture.py at setup time: nothing on
# the stock System 6 volume is both a standalone application and visually
# distinctive, and generating it keeps the 68K and the resource map reviewable
# instead of committing an opaque blob.

TEST_NAME := AppleTalk AFP end-to-end fork fidelity
TEST_DESC := HFS image -> cp -> AppleDouble -> AFP -> guest launches the app from its resource fork -> round-trip compare.

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_SETUP := cp "$(TEST_DATA)/systems/system_6_0_8_20mb_8_24gc.img" "$(TEST_TMPDIR)/hd.img" && mkdir -p "$(WORK_DIR)/share" "$(WORK_DIR)/again" && python3 appletalk-afp-e2e/make-fixture.py "$(WORK_DIR)/share"

TEST_ARGS := hd=$(TEST_TMPDIR)/hd.img

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
