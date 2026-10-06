# Integration test: a warm reset blanks VRAM video until the ROM reprograms it (#236).
#
# machine.reset is a warm /RESET: VRAM survives, as on the hardware, but the
# video controller comes out of reset with sync off, so a real monitor goes
# dark until the ROM programs it again.  The JMFB NuBus card (IIcx/IIx/IIfx
# default), the DAFB (Quadra 700/900/950) and the Civic (Quadra 660AV/840AV)
# kept scanning the old VRAM picture for 1-5 s of emulated time instead.  They
# now present a black raster from the reset until the ROM's own video set-up
# (restart-video-cold is the power-cycle half).

TEST_NAME := Reset blanks VRAM video
TEST_DESC := after machine.reset the JMFB, DAFB and Civic show a blank frame, not the pre-reset one, and the ROM draws again

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := model=plus

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
