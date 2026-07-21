# Integration test configuration: IIcx + Display Card 8•24 GC — 16-bpp
# (Thousands) rasterizers, reached the only way the real card reaches 16 bpp:
# a runtime VidComm mode switch (boot depths cap at 8 under the old ROM Slot
# Manager).  Exact RGB555 framebuffer words for the boolean and arithmetic
# cores.

TEST_NAME := IIcx Display Card 8•24 GC — 16-bpp via VidComm (exact FB words)
TEST_DESC := VidComm switch to 16 bpp + copy/xor/blend cores, byte-exact

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_ARGS := model=iicx ram=8192
