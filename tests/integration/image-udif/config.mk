# Integration test: UDIF (.dmg) images decode transparently on open.
#
# The `udif` unit suite covers the trailer/block-map parser and the chunk
# decoders in isolation; this covers the plumbing they hang off — koly
# detection at EOF, plist retrieval, materialisation to the storage-cache
# scratch image, and the per-table CRC-32 verification that guards it — by
# round-tripping a real HFS volume through the format and mounting the result.
#
# make-fixture.py builds the .dmg at setup time from a raw image already in
# tests/data (which is fetched, not committed), so no binary fixture is added
# to the tree.  It is written from the format spec rather than from the
# reader, so agreement between the two is evidence, not tautology.

TEST_NAME := storage.udif
TEST_DESC := Decode a UDIF (.dmg) image on open and mount the volume inside it

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_SETUP := python3 image-udif/make-fixture.py "$(TEST_DATA)/systems/system_6_0_8_20mb_8_24gc.img" "$(WORK_DIR)/fixture.dmg"

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
