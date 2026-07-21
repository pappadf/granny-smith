# Integration test configuration: IIcx + Display Card 8•24 GC — System 6.0.8
# "Marathon" HD image, 8 bpp.
#
# Same boot shape as iicx-824gc-accel8 (machine.nubus.video_mode seeds the
# slot PRAM to 640×480 8 bpp so the decl-ROM video driver brings the screen
# up in colour), but from the marathon HD image — a 20 MB System 6.0.8 volume
# with Marathon v1.2 installed whose Finder desktop opens the "Marathon ƒ"
# folder window at boot.  The test then double-clicks Marathon, runs it to
# its main menu — stopping inside the window before the game's self-running
# demo kicks in (see test.script) — opens PREFERENCES, sets the Graphics
# options (High Resolution / 100% / 256 colors / Normal), accepts with OK, and
# starts a new game: through the "Arrival" cut scene into live gameplay.  Every
# scene is pixel-exact.
#
# (256 colors is Marathon 1.2's supported depth at 640×480.  Selecting
# "Thousands" instead drives the 8•24 GC into its faithful double-wide 16-bpp
# mode, which the game mishandles into a heap-corruption bomb — real hardware
# behaviour, investigated in tmp/marathon-16bpp-bomb-findings.md, not exercised
# by this test.)

TEST_NAME := IIcx Display Card 8•24 GC — Marathon boot + new game at 8 bpp
TEST_DESC := Boot IIcx + 8•24 GC at 8 bpp, launch Marathon, set Preferences to High-Res/100%/256/Normal, OK, Begin New Game through the Arrival cut scene into gameplay; pixel-exact

TEST_ROM := roms/iix-iicx-se30-97221136.rom

# The harness creates the IIcx with 8 MB; the script re-boots with the 8•24 GC
# card selected (video_card can't be passed as an arg) and attaches the SCSI HD.
TEST_ARGS := model=iicx ram=8192
