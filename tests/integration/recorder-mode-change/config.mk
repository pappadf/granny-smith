# Integration test: the recorder across guest mode changes.
#
# A IIcx on a 13" RGB monitor, its slot PRAM seeded for 8 bpp, powers on at
# 1 bpp and the driver switches to 8 bpp during the boot: every indexed
# depth is 8 bpp + palette in the file, so the recording stays one file.
# A Quadra 700 on a two-page monitor powers on at 640x480 and the DAFB
# driver then sets 1152x870: a mode larger than the canvas closes the file
# and the recording continues in a second segment (name-2.mkv).  See
# docs/internals/core/record/record.md.

TEST_NAME := Recorder — mode changes (depth stays one file, a larger mode is a new segment)
TEST_DESC := IIcx 1 -> 8 bpp in one file; Q700 640x480 -> 1152x870 in two segments; each file decodes to the screen

TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_ARGS := model=iicx ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
