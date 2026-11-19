# Integration test: a SCSI card and a display card in an 8500's PCI sockets
#
# Two PCI devices this repository first modelled as the Apple Network
# Server's soldered parts -- the Symbios 53C825A and the Cirrus 54M30 -- in
# the sockets of a Power Macintosh 8500, as PC cards: the SCSI card with
# the bus it brings ("pci_1", machine.pci_1), the Cirrus beside it.  See
# test.script.

TEST_NAME := TNT PCI SCSI and display cards
TEST_DESC := A Power Macintosh 8500 with a Symbios 53C825A SCSI card (and its own SCSI bus) and a Cirrus 54M30 in its PCI sockets boots System 7.6 to the unchanged Finder desktop, its card bus takes drives and survives a checkpoint

TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

TEST_ARGS := model=pm8500 ram=32768

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
