# Integration test: what a configuration seeds, and what survives.
#
# A Quadra 900 built from a rich document -- two NuBus cards, three drives on
# both SCSI buses, AppleTalk inactive, a startup disk at ID 2, the built-in
# video's startup mode -- has the seeded parameter memory at instruction 0.
# The guest then changes all three records, as its control panels would;
# machine.reset(), machine.restart and a checkpoint round trip keep the
# guest's choices (nothing seeds again) and the machine the document built.

TEST_NAME := Configuration seeding and persistence
TEST_DESC := A rich document's seeded PRAM, and the guest's changes kept across reset, restart and a checkpoint

TEST_ROM := roms/q700-q900-420dbff3.rom
TEST_ARGS := model=q900

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
