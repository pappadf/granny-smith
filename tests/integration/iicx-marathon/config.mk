# Integration test configuration: IIcx + Display Card 8•24 GC — System 6.0.8
# "Marathon" HD image, 8 bpp.
#
# Same boot shape as iicx-824gc-accel8 (machine.nubus.video_mode seeds the
# slot PRAM to 640×480 8 bpp so the decl-ROM video driver brings the screen
# up in colour), but from the marathon HD image — a 20 MB System 6.0.8 volume
# with Marathon v1.2 installed whose Finder desktop opens the "Marathon ƒ"
# folder window at boot.  The test then double-clicks Marathon and runs it to
# its main menu — stopping inside the window before the game's self-running
# demo kicks in (see test.script).  Both scenes are pixel-exact.

TEST_NAME := IIcx Display Card 8•24 GC — Marathon image boot at 8 bpp
TEST_DESC := Boot IIcx + 8•24 GC at 8 bpp from the Marathon HD image to the Finder desktop, then launch Marathon to its main menu; pixel-exact

TEST_ROM := roms/IIcx.rom

# The harness creates the IIcx with 8 MB; the script re-boots with the 8•24 GC
# card selected (video_card can't be passed as an arg) and attaches the SCSI HD.
TEST_ARGS := model=iicx ram=8192
