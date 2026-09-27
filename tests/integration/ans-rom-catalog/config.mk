# Integration test: every Open Firmware Apple Network Server ROM boots.
#
# The core's ROM table lists five ANS ROMs.  Three share the header sum
# $962F6C13 (Open Firmware 1.1.20.1, 1.1.22 and 2.26NT) and one (2.26B6)
# shares $9630C68B with the Power Macintosh 9500 v2 ROM, so only the full
# content id — header sum plus ConfigInfo 64-bit sum — tells them apart.
# This row boots each Open Firmware ROM to its `0 >` prompt on ttya and checks
# the core identified it as itself.  The 2.0 prototype (Mac OS) is covered by
# suite-ans; 1.1.22 on the 700 by ans-device-tree.

TEST_NAME := ANS ROM catalog
TEST_DESC := Boots every Open Firmware ANS ROM to the 0 > prompt and checks its content id

TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=32768

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
