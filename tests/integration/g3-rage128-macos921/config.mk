# Integration test: Mac OS 9.2.1 on an ATI Rage 128 GL in a beige G3
#
# PCI Phase 4.  The G3 with the card in slot A1 and nothing on its built-in
# Rage Pro port (monitor "none"), so the card is the machine's only display:
# its FCode and ndrv bring up the desktop, and 9.2.1's ATI extensions
# accelerate QuickDraw through the card's Concurrent Command Engine with
# their own microcode, in PM4 mode 7 (PIO packets plus bus-mastered indirect
# buffers through the PCI GART).  The disk is gs-test-data's
# systems/mac_os_9_2_1_516mb_g3.img, as suite-gossamer boots it.

TEST_NAME := G3 Mac OS 9.2.1 on a Rage 128
TEST_DESC := Mac OS 9.2.1 boots to the desktop on an ATI Rage 128 GL as the beige G3's only display, QuickDraw accelerated through the CCE

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

TEST_ARGS := model=pmg3dt ram=65536

# One boot of ~12 G instructions.
TEST_TIMEOUT := 2400

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
