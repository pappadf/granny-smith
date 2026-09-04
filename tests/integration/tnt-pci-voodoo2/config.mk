# Integration test: the 3dfx Voodoo2 enumerates and survives bring-up
# (proposal-pci-3dfx-voodoo2, milestone 3b).
#
# The first ROM-less socket card on the generic PCI core: no expansion
# ROM, no FCode, no ndrv.  The acceptance shape is Apple's own
# specification of the case ("Designing PCI Cards and Drivers", revised
# 1999): Open Firmware sizes and assigns the single BAR, publishes a bare
# node generated from config space, and leaves Memory Space Enable CLEAR
# for the disk-loaded driver to set — which is exactly what this row
# observes the guest's real firmware doing.
#
# The bring-up replay (glide-init.script) is 3dfx's own initialisation
# order, transcribed from the vendor's released Glide source
# (glide2x/cvg/init/sst1init.c — sequence and constants, no code copied),
# with the postcondition of each step asserted in place: the first step
# whose postcondition fails is the finding.
#
# The idle contract runs through everything: every wait is a bounded
# poll, so a busy bit that never clears is a test failure here and not a
# hung suite (V2 spec p.128 §12.3; proposal §4.4).

TEST_NAME := TNT PCI Voodoo2
TEST_DESC := 3dfx Voodoo2 in a pm7500 socket: ROM-less enumeration, vendor config space, Glide bring-up replay

# 4 MB Power Macintosh 7500/8500/9500 ROM v1 (stored checksum 0x96CD923D)
TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

TEST_ARGS := model=pm7500 ram=32768

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
