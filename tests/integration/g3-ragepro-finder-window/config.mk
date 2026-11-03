# Integration test: Finder window drawing on the beige G3's built-in Rage Pro
#
# Mac OS 9.2.1 on the G3's own ATI Rage Pro (the built-in port's default
# monitor, no PCI card), as the web UI's G3 runs it.  ATI's accelerator
# draws a Finder window's content as colour host data a cache line at a time
# (dcbz, stmw, dcbst) and scrolls it with a screen-to-screen blit started
# through DST_WIDTH_HEIGHT.  The disk is gs-test-data's
# systems/mac_os_9_2_1_516mb_g3.img, as suite-gossamer boots it.

TEST_NAME := G3 Finder window on the Rage Pro
TEST_DESC := Mac OS 9.2.1 opens, shrinks, scrolls and grows a Finder window on the beige G3's built-in Rage Pro

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

TEST_ARGS := model=pmg3dt ram=65536

# One boot of ~24 G instructions, then ~30 G of Finder work.
TEST_TIMEOUT := 3000

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
