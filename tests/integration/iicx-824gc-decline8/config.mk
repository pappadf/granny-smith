# Integration test configuration: IIcx + Display Card 8•24 GC — the 8-bpp
# DIFFERENTIAL TEST ORACLE (proposal §4.1).
#
# Identical boot to iicx-824gc-accel8 except gc.force_decline: every drawing
# func declines and the ROM QuickDraw path renders the whole colour scene.
# The screen must match the SAME reference PNG the accelerated test matches —
# accel ≡ ROM at 8 bpp, pixel for pixel.

TEST_NAME := IIcx Display Card 8•24 GC — 8 bpp force_decline differential oracle
TEST_DESC := Same 8-bpp boot as iicx-824gc-accel8 but every drawing func declines; the ROM path must render identical pixels

TEST_ROM := roms/IIcx.rom

TEST_ARGS := model=iicx ram=8192
