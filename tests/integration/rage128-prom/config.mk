# Integration test: the ATI Rage 128 GL's expansion ROMs are identified
#
# PCI Phase 4, milestone 4a: before the card exists, the PROM catalogue must
# already know its ROMs -- and the ROMs that are NOT its ROMs.  Three
# outcomes, each with its own message:
#   * the two retail Rage 128 GL Mac ROMs (Xclaim VR 128, Nexus 128) are
#     recognised and provide the `rage128` card kind;
#   * the two Rage 128 Pro AGP Mac ROMs are genuine Open Firmware ROMs for a
#     sibling chip: recognised and REFUSED, saying what they are;
#   * the PC Rage 128 GL VGA BIOS is an x86 option ROM: rejected as such.
#
# The refused fixtures live in roms/refused/ of the test data, outside the
# flat roms/ directory whose every file rom-catalog requires recognised.

TEST_NAME := Rage 128 expansion ROMs
TEST_DESC := catalog.proms identifies the Rage 128 GL ROMs and refuses the Pro and PC ROMs

TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
