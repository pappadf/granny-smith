# Integration test configuration: IIcx + Display Card 8•24 GC — 8-bpp colour
# rasterizer math, driven straight through the DrawMultiObject queue.
#
# Covers the qd-transfer-modes §3.1 arithmetic transfer modes (blend, addPin,
# addOver, subPin, transparent, adMax, subOver, adMin — component math on the
# CLUT, result through the card-side Color2Index) and op $74 opRGBPat
# (MakeRGBPat's 2×2 ordered dither, Patterns.a DthrTbl13).  Every result is a
# hand-computed exact framebuffer byte on the power-on ramp CLUT.

TEST_NAME := IIcx Display Card 8•24 GC — 8-bpp arithmetic modes + RGBPat (exact FB bytes)
TEST_DESC := Arithmetic transfer modes ($20-$27) + MakeRGBPat dither, poke-driven, byte-exact

TEST_ROM := roms/IIcx.rom

TEST_ARGS := model=iicx ram=8192
