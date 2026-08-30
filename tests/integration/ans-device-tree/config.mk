# Integration test: the Apple Network Server's Open Firmware device tree
# (proposal-apple-network-server-500-700 §6, §7, ladder rungs S5-S7)
#
# THE ACCEPTANCE ORACLE.  We do not construct a device tree; Open Firmware
# 1.1.22 probes our registers and builds one.  If `dev / ls` does not match
# Apple's own published Listing 6-1, the HARDWARE MODEL is wrong — which
# makes this the strongest single statement about the machine available,
# and it is published by Apple, for this machine, in a developer note.
#
# It matters twice over because of what comes after.  AIX's configuration
# methods walk the tree and match each node's `name` against its ODM
# Predefined Devices database: "the name must be stored in the Predefined
# Devices (PdDv) database of the ODM."  A device whose node name does not
# match a PdDv entry is not configured, regardless of whether the hardware
# works — and the AIX 4.1.5 install media's own `devices.pci.gc` fileset
# enumerates the six children it expects under `gc` by name.
#
# The row drives Open Firmware over the SCC serial console, which is also
# how ladder rung S6 is observed: the `0 >` prompt on ttya.
#
# INPUT IS SENT IN SHORT CHUNKS.  Open Firmware's console loses characters
# from a long burst delivered in one go; feeding it eight at a time with a
# bounded run between chunks is reliable, and costs nothing.

TEST_NAME := ANS device tree
TEST_DESC := Boots Open Firmware 1.1.22 and matches its device tree, node properties and aliases against Apple's Listing 6-1

# 4 MB Apple Network Server 500/700 ROM, Open Firmware 1.1.22.
TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=32768

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
