# Integration test: a card's ROM named in the boot document is enough.
#
# The ROM lives in a directory with no card ROMs beside it, so the platform
# offers none: Apple's declaration ROM for a named card comes only from the
# document's vrom= -- the web's New Machine dialog boots a card that way --
# and without it the card runs the emulator's substitute.

TEST_NAME := A named card boots from the document's vrom= alone
TEST_DESC := video_card= plus vrom= boots Apple's ROM with nothing offered; video_card= alone boots the card's substitute ROM

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
