# Integration test: the beige Power Macintosh G3's Open Firmware device tree.
#
# THE ACCEPTANCE ORACLE for the Gossamer hardware model (ladder rung G6).
# We do not construct a device tree; Open Firmware 2.4 probes our registers
# and builds one.  Every property below is compared against a full
# `dump-device-tree` taken on a real Rev C beige G3, so a mismatch means the
# hardware model — not the firmware — is wrong.

TEST_NAME := Gossamer device tree
TEST_DESC := Boots Open Firmware 2.4 on the beige G3 and matches its device tree against a real machine's dump

# 4 MB Power Macintosh G3 ROM Rev C ($077D.45F2, Open Firmware 2.4).
TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

TEST_ARGS := model=pmg3dt ram=65536

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
