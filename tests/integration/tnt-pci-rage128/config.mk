# Integration test: the ATI Rage 128 GL enumerates, Open Firmware builds its
# node from the card's own FCode, and its ndrv draws the Mac OS desktop
#
# PCI Phase 4, milestone 4b.  A Power Macintosh 9500 with the card in socket
# A1.  There is no Apple dump of this card's node to compare against (the
# Mach64 row has TN1062); the acceptance target is the node the card's OWN
# FCode builds, read back with Open Firmware 1.0.5's `.properties` over the
# serial console, and checked against the decode of that FCode (what it
# publishes, from which register answers).

TEST_NAME := TNT PCI Rage 128
TEST_DESC := ATI Rage 128 GL in a pm9500 socket: config header, the FCode-built node, sense and DDC, and the ndrv's desktop

# 4 MB Power Macintosh 7500/8500/9500 ROM v1 (stored checksum 0x96CD923D)
TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

TEST_ARGS := model=pm9500 ram=32768

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
