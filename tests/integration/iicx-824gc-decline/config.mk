# Integration test configuration: IIcx + Display Card 8•24 GC — the
# DIFFERENTIAL TEST ORACLE (proposal §4.1).
#
# Identical boot to iicx-824gc-accel (accelerator ON, real .GraphAccel/GCQD
# bring-up, RTC pinned) except gc.force_decline is set, so the card declines
# every drawing func ($2D SetPort / $15 StretchBits / $30 FontDownload) and the
# ROM QuickDraw path renders the whole scene.  The screen must match the SAME
# reference PNG the accelerated test matches — proving accel path ≡ ROM path
# pixel-for-pixel on the identical guest scene.  Any future rasterizer change
# that diverges from the ROM oracle fails exactly one of the two tests.

TEST_NAME := IIcx Display Card 8•24 GC — force_decline differential oracle
TEST_DESC := Same boot as iicx-824gc-accel but every drawing func declines; the ROM path must render the identical pixels

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_ARGS := model=iicx ram=8192
