# Integration test: TNT PCI slot topology and the empty-socket contract
# (proposal-pci-architecture, Phase 1).
#
# The gate for the generic PCI core's TNT integration: the machine's slot
# table reaches the profile, the object model and the runtime from one
# declaration; a registered device answers config cycles and an
# unpopulated IDSEL reads all-ones; Open Firmware sizes and assigns
# Control's BARs through the generic header exactly as the hand-rolled
# model made it; empty PCI memory space still faults recoverably; and the
# staged-card surface validates what it is handed.

TEST_NAME := TNT PCI slots
TEST_DESC := Slot topology, config-cycle contract and empty-socket semantics on the 7500/9500

# 4 MB Power Macintosh 7500/8500/9500 ROM v1 (stored checksum 0x96CD923D)
TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

TEST_ARGS := model=pm7500 ram=32768

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
