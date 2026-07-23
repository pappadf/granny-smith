# Integration test configuration: IIcx + generic 24AC ("24ac")
# Welcome-screen boots on the generic Display Card 24AC sibling at two
# staged geometries (proposal-generic-nubus-vrom.md stage 2).

TEST_NAME := IIcx 24AC (GS generic vROM)
TEST_DESC := Boot Welcome on the generic 24AC kind at 640x480 and 832x624.

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_ARGS := model=iicx ram=8192 fd=$(TEST_DATA)/systems/System_7_0_1.image
