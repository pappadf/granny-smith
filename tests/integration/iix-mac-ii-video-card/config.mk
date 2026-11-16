# Integration test configuration: IIx + Macintosh II Video Card ("Toby")
#
#   Part A - the TFB and Bt453 register decode, driven through memory
#            poke/peek with no OS: the inverted data bus, the depth and page
#            registers, the palette as the driver addresses it, the VBL
#            interrupt control.
#   Part B - a full boot of System 7.0.1 to an 8 bpp colour Finder on Apple's
#            342-0008 declaration ROM, matched pixel-exact.

TEST_NAME := IIx Macintosh II Video Card
TEST_DESC := Macintosh II Video Card: TFB/RAMDAC register decode + 8bpp colour desktop on Apple's ROM.

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_ARGS := model=iix ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
