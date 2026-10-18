# Integration test: the PDM built-in video on a monochrome monitor (S1018-58).
#
# The ROM's Sonora driver loads its grays into Ariel's blue channel only, so
# the 15" Portrait's picture is that channel as luminance.  Rendering the raw
# RGB drew the gray screen in black and pure blue.  One ROM-stage boot of a
# PM 6100 on the Portrait, matched against a black-and-white gray screen
# (the floppy icon and the cursor left out of the compare).

TEST_NAME := PDM Portrait in grays
TEST_DESC := pm6100 on the 15" Portrait draws its ROM gray screen in grays, not blue

TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom
TEST_ARGS := model=pm6100 monitor=portrait

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
