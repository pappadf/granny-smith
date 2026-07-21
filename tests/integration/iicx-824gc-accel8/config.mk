# Integration test configuration: IIcx + Display Card 8•24 GC — accelerator ON
# at 8 bpp (the other shipped boot configuration).
#
# Same boot as iicx-824gc-accel but with machine.nubus.video_mode seeding the
# slot PRAM to 640×480 8 bpp, so the decl-ROM video driver brings the screen
# up in colour and GCQD queues 8-bpp state (real pixValues, hilite selection,
# colour CLUT).  The card's depth-parameterized rasterizers draw the desktop;
# the reference PNG must match pixel-exactly.  iicx-824gc-decline8 renders the
# IDENTICAL scene via the ROM path against the SAME reference — the two
# together are the 8-bpp differential oracle (proposal §4.1).

TEST_NAME := IIcx Display Card 8•24 GC — accelerator ON at 8 bpp
TEST_DESC := Boot IIcx + 8•24 GC at 8 bpp with the accelerator drawing; colour desktop pixel-exact

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_ARGS := model=iicx ram=8192
