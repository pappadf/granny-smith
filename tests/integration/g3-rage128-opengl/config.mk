# Integration test: OpenGL on an ATI Rage 128 GL under Mac OS 9.2.1
#
# PCI Phase 4.  The G3 of g3-rage128-macos921 (the card the only display),
# plus Apple's OpenGL SDK 1.2 CD (gs-test-data apps/OpenGL_SDK_1.2.img_.bin,
# MacBinary + NDIF, attached as it is).  Two of the SDK's GLUT "book"
# samples run on ATI's OpenGL renderer, which draws through the card's 3D
# engine: checker (textures, perspective), mipmap (mip levels) and fog
# (exponential fog, Z).

TEST_NAME := G3 OpenGL on a Rage 128
TEST_DESC := Apple OpenGL's ATI renderer draws GLUT samples through the Rage 128 GL's 3D engine under Mac OS 9.2.1

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

TEST_ARGS := model=pmg3dt ram=65536

# One boot, the Finder walk to the samples, three GL programs: ~70 G
# instructions.
TEST_TIMEOUT := 6000

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
