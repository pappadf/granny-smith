# Integration test configuration: IIfx + Display Card 8•24 GC — System 7
# Finder boot.
#
# The deepest computed-card-compatibility check (nubus_card_fits_socket):
# the IIfx declares no per-card whitelist, and the 8•24 GC — developed and
# pixel-validated on the IIcx — must boot the OSS-family IIfx all the way
# to the System 7 Finder.  The System boot walks the real Slot Manager
# path: format-block probe, sResource walk, and the card's decl-ROM video
# driver (RISC serial command stream), exactly as on the IIcx.
#
# NOTE the boot flow: the card is picked with the `video_card=` startup
# argument, NOT the script-side `machine.nubus.video_card` + machine.boot
# dance the IIcx suites use — on the IIfx a scripted machine.boot never
# boots a disk (the SWIM-IOP does not see post-boot floppy inserts, and
# Mac OS SCSI-HD boot is not functional on the IIfx at all).  The ROM-only
# phase also never runs the IIfx ROM's slot scan — it blind-paints a dummy
# screen at the conventional slot-9 VRAM+$A00 address, which the GC's
# power-on scanout (its Am29000 DRAM framebuffer) doesn't display; only
# the OS boot exercises the real driver path this test pins.

TEST_NAME := IIfx Display Card 8•24 GC — System 7 Finder
TEST_DESC := Boot the IIfx with the 8•24 GC (computed compatibility) from the System 7.0.1 floppy to a pixel-exact Finder desktop

TEST_ROM := roms/iifx-4147dd77.rom

# Same floppy + RAM as iifx-boot; video_card= stages the pick before
# machine creation (the headless twin of web2's pre-boot
# machine.nubus.video_card write).
TEST_ARGS := model=iifx ram=16384 fd=$(TEST_DATA)/systems/System_7_0_1.image video_card=824gc
