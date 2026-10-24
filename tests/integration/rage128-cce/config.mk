# Integration test: the ATI Rage 128 GL's Concurrent Command Engine
#
# PCI Phase 4, milestone 4e.  The CCE is driven as the SDK's chapter 5
# drives it — microcode upload, PM4_BUFFER_CNTL, packets through the PIO
# FIFO, a bus-mastered ring in guest RAM reached through the PCI GART, an
# indirect buffer — and every assertion is an EQUALITY: VRAM contents read
# back through linear aperture 0, the ring's read pointer in the register
# and in guest RAM.  No guest code runs: the row assigns the card's BARs
# itself through Bandit's config ports.

TEST_NAME := Rage 128 CCE
TEST_DESC := Rage 128 GL command packets through the PIO FIFO, a bus-mastered ring via the PCI GART, and the indirect buffer, checked as VRAM and RPTR equalities

# 4 MB Power Macintosh 7500/8500/9500 ROM v1 (stored checksum 0x96CD923D)
TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
