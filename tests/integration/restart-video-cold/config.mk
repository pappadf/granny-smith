# Integration test: a power cycle does not keep showing the old picture (#236).
#
# machine.restart is a power cycle: main RAM is cleared, so video that
# lives in main RAM changes at once.  Video with its own VRAM -- the JMFB
# NuBus card (IIcx/IIx/IIfx default), the DAFB (Quadra 700/900/950) and the
# Civic (Quadra 660AV/840AV) -- kept scanning the old VRAM for 1-5 s of
# emulated time, until the ROM reprogrammed the controller.  VRAM is now
# lost with the power, and the raster comes up black like a cold build's.
# The same held for the SE/30's built-in video and the 8*24 GC, which had no
# power-cycle hook at all; a power cycle now has a bus-level default on NuBus
# and PCI (zero the card's VRAM, blank the raster), which the 24AC, Control
# and the Mach64 rows also take.

TEST_NAME := Restart blanks VRAM video
TEST_DESC := after machine.restart no VRAM display (JMFB, DAFB, Civic, SE/30, 24AC, 8*24 GC, Control, Mach64) shows the pre-restart frame

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := model=plus

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
