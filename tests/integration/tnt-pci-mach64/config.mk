# Integration test: the Apple Accelerated PCI Graphics Card enumerates
# (proposal-pci-mach64-gx-spinnaker, milestone 2b).
#
# The first pluggable card on the generic PCI core, seated in socket A1 of
# a Power Macintosh 9500.  The acceptance target is not a golden we made
# up: it is Apple's own dump of this card's device-tree node from a REAL
# 9500 under Open Firmware 1.0.5 (Technote 1062, "Fundamentals of Open
# Firmware, Part II").  Every structural fact asserted below appears in
# that dump.
#
# It also pins the expansion-ROM provisioning path the card depends on:
# identification by content, the x86-option-ROM rejection, and strict boot
# validation for a card that cannot resolve one.

TEST_NAME := TNT PCI Mach64 GX
TEST_DESC := ATI Mach64 GX in a pm9500 socket: config header, expansion ROM, aperture and monitor sense

# 4 MB Power Macintosh 7500/8500/9500 ROM v1 (stored checksum 0x96CD923D)
TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

TEST_ARGS := model=pm9500 ram=32768

# Two negative fixtures, built here rather than committed: a structurally
# valid PC/x86 option ROM (code type 0) and a valid Open Firmware ROM that
# no catalog row claims.  Both are what a user's mistake actually looks
# like, and neither is interesting enough to store as a binary.
TEST_SETUP := python3 tnt-pci-mach64/make-fixtures.py "$(WORK_DIR)"

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
