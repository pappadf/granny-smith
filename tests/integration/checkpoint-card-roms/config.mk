# Integration test: a checkpoint carries its cards' ROMs
#
# Step 1 boots an IIcx with two cards of one kind (the 24AC in sockets $9 and
# $A, their declaration ROMs picked from the catalog) and a Power Macintosh
# 9500 with a Mach64 in PCI slot 1 (its FCode PROM picked from the catalog),
# and saves a checkpoint of each.  Step 2 restores both in a fresh process
# whose ROM directory holds no card ROM at all: the cards come back from the
# bytes in the checkpoint, not from a catalog lookup.

TEST_NAME := Checkpoint card ROMs
TEST_DESC := Two cards of one kind and a PCI slots= card restore without their ROM files

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
