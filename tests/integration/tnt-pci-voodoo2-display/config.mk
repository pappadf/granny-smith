# Integration test: the Voodoo2 pass-through switch against a live
# desktop (proposal-pci-3dfx-voodoo2, milestone 3d).
#
# A pm8500 boots System 7.6 to the Finder on its soldered Control video
# with a Voodoo2 seated in socket A1 — the machine a real owner had.
# The card then takes the monitor (fbiInit0[0], video timing programmed,
# unblanked, outputs driven), shows its own rendering, and releases it —
# and the desktop must come back BYTE-IDENTICAL, the strongest available
# statement that the takeover was non-destructive.  The switch is
# nothing but pci_primary_display_card() re-resolving per frame against
# the card's own predicate; no generic-layer change exists to test.
#
# A checkpoint taken while the card is driving must restore to the
# identical framebuffer checksum.

TEST_NAME := TNT PCI Voodoo2 display
TEST_DESC := pm8500 + 7.6 on Control with a Voodoo2 in A1: the pass-through switch takes and releases the monitor

TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

# Copied into WORK_DIR because the System writes to the volume during boot.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_6_170mb_24ac.img" "$(WORK_DIR)/hd.img"

TEST_ARGS := model=pm8500 ram=32768

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
