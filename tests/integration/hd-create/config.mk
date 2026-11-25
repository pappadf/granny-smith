# Integration test configuration: files.create and the drive models
# Verifies files.create for each kind (model-aware hard disk sizes) and the
# hd models listing

TEST_NAME := files.create
TEST_DESC := Tests files.create: hard disks by model, human and exact size; floppies; ProFiles; refusals

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/iix-iicx-se30-97221136.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
