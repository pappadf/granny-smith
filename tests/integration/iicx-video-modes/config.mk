# Integration test configuration: IIcx Video Modes
# Cycle the Apple Macintosh Display Card 8•24 (JMFB) through every
# supported pixel depth on the 13" RGB monitor sense — 1, 2, 4, 8, and
# 24 bpp at 640x480 — and verify the framebuffer renders correctly at
# each.  See tests/integration/iicx-video-modes/test.script for the
# detailed setup, and docs/pram.md §9 for the reasoning behind the
# direct-register-programming approach (vs. PRAM-driven boot mode,
# which the IIcx ROM does not honour at the boot-icon stage).

TEST_NAME := IIcx Video Modes
TEST_DESC := Cycles JMFB through 1/2/4/8/24 bpp via direct CLUTPBCR/RowWords programming and verifies the renderer at each depth.

TEST_ROM := roms/IIcx.rom

# 8 MB RAM lets the JMFB driver complete PrimaryInit cleanly (matches
# the iicx-boot / iicx-floppy budgets).  No floppy media — the test
# anchors at the boot ROM's "insert disk" floppy/? icon, which is
# painted at 1 bpp by PrimaryInit before any user-mode code runs.
TEST_ARGS := model=iicx ram=8192
