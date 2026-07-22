# Integration test configuration: IIcx + generic 8•24 GC ("8_24gc")
# System 6.0.8 accelerator bring-up on the generic GC sibling: the
# "8•24 GC" extension must attach against the GS ROM's identity records
# (BoardId $2C, RevLevel, Display_Video_Apple_MDCGC) and turn the
# accelerator on (proposal-generic-nubus-vrom.md sec. 7.1 / 11.1).
#
# KNOWN STAGE-2 GAP (sec. 7.2): the visible-Finder re-point into the
# super-slot DRAM framebuffer needs the real card's quadrant state
# machine, so this suite asserts the bring-up ladder, not pixels.

TEST_NAME := IIcx 8.24 GC (GS generic vROM)
TEST_DESC := GC accelerator bring-up (attach/boot/arm/gc-on) on the generic kind.

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_ARGS := model=iicx ram=8192
