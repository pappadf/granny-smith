# Integration test: disk images stored as UDIF (.dmg).
#
# Converts a real HFS disk to a GS-profile UDIF with files.convert, checks it
# is much smaller, verifies its checksums, mounts the volume inside it in
# place, converts it back to raw byte-for-byte, and creates a blank 2 GB
# .dmg that costs a few KB.  The writer and reader are unit-tested in
# isolation (udif_writer suite); this is the object-model plumbing over them.

TEST_NAME := storage.udif-store
TEST_DESC := Convert a disk to UDIF and back, verify it, and create a blank UDIF disk

TEST_ROM := roms/iix-iicx-se30-97221136.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
