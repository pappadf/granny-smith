# Integration test: the NuBus display cards' substitute-ROM variants
#
# Every NuBus display card with a substitute declaration ROM (the 8•24, the
# 24AC, the 8•24 GC) is listed twice in catalog.profile: the card itself,
# which runs Apple's declaration ROM when it is offered (and falls back to
# the substitute when it is not), and "<card> · Substitute ROM"
# (`<id>_generic`, `variant_of` the card), which always runs the emulator's
# generated ROM and needs no file.  A configuration document naming the
# variant seats the card with rom=substitute, even with Apple's ROM offered
# (#346).  The SE/30's built-in video, no slot card, takes the choice as
# the machine option video_rom (apple | substitute).  The harness offers the .vrom files beside the ROM, so Apple's
# ROMs are stored here.  No guest code runs.

TEST_NAME := Display card generic video ROM variants
TEST_DESC := catalog.profile lists "substitute-ROM variants of the NuBus display cards; a document naming one boots the substitute ROM with Apple's offered.

TEST_ROM := roms/iix-iicx-se30-97221136.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
