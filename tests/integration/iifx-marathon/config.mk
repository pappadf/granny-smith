# Integration test configuration: IIfx + Display Card 8•24 GC — System 6.0.8
# "Marathon" HD image, 8 bpp.
#
# The IIfx twin of iicx-marathon: same marathon HD image and 8•24 GC card,
# booted on the OSS-family IIfx via the scripted machine.boot dance (the
# IIfx SCSI-boots Mac OS fine; the older "cannot disk-boot from script"
# note predates the A/UX-era SCSI fixes).  Exercises the same Marathon
# v1.2 launch -> Preferences -> Begin New Game flow with IIfx-scaled
# real-time waits (40 MHz vs 16 MHz: ~2.5x the instructions per second).

TEST_NAME := IIfx Display Card 8•24 GC — Marathon boot + new game at 8 bpp
TEST_DESC := Boot IIfx + 8•24 GC at 8 bpp, launch Marathon, set Preferences to High-Res/100%/256/Normal, OK, Begin New Game through the Arrival cut scene into gameplay; pixel-exact

TEST_ROM := roms/4147DD77-IIfx.rom

# The harness creates the IIfx with 16 MB; the script re-boots with the
# 8•24 GC card selected and attaches the SCSI HD.
TEST_ARGS := model=iifx ram=16384
