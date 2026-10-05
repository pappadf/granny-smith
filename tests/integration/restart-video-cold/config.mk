# Integration test: a power cycle does not keep showing the old picture (#236).
#
# machine.restart is a power cycle: main RAM is cleared, so video that
# lives in main RAM changes at once.  Video with its own VRAM -- the JMFB
# NuBus card (IIcx/IIx/IIfx default), the DAFB (Quadra 700/900/950) and the
# Civic (Quadra 660AV/840AV) -- kept scanning the old VRAM for 1-5 s of
# emulated time, until the ROM reprogrammed the controller.  VRAM is now
# lost with the power, and the raster comes up black like a cold build's.

TEST_NAME := Restart blanks VRAM video
TEST_DESC := after machine.restart the JMFB, DAFB and Civic no longer show the pre-restart frame

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := model=plus

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
