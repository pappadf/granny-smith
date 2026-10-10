# Integration test: the ATI Rage 128 GL's 2D draw engine
#
# PCI Phase 4, milestone 4d.  The engine is driven exactly as a driver
# drives it — DP_GUI_MASTER_CNTL, the colours and the trajectory written
# through the register aperture, then an initiator — and every assertion
# is an EQUALITY on VRAM contents read back through the linear aperture,
# not a picture.  No guest code runs: the row assigns the card's BARs
# itself through Bandit's config ports, so it is fast and exact.

TEST_NAME := Rage 128 2D engine
TEST_DESC := Rage 128 GL rectangles, blits, transparency, mono expansion, brushes, ROP3, write mask, scissors and lines, checked as VRAM equalities

# 4 MB Power Macintosh 7500/8500/9500 ROM v1 (stored checksum 0x96CD923D)
TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
