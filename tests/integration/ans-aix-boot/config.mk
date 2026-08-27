# Integration test: booting AIX 4.1.5 for Apple Network Servers
# (proposal-apple-network-server-500-700 §6, ladder rungs S9-S11)
#
# The first commercial Unix this repository boots as a machine's PRIMARY
# operating system, and the first guest that is neither Apple's nor open:
# AIX 4.1.5 for the Network Server has no public source, so when it
# misbehaves the TNT reflex — disassemble the guest — is not available.
# What the machine gives instead is three narrators, and this row reads all
# three: the front-panel LCD, the Open Firmware serial console, and the
# device tree.
#
# THE BOOT PATH IS APPLE'S OWN, not an invention.  The row does not type a
# boot command: it sets the front keyswitch to Service on a machine with a
# blank non-volatile store, which is exactly the documented condition —
# "If Open Firmware detects the key in the service position on a Network
# Server that has never been booted before, it will automatically attempt
# to find a diagnostic floppy or Install CD to boot from."
#
# WHAT THE MEDIA IS.  `AIX_415_Install_CD.iso`, volume AIX_INSTALLATION_CD,
# an ISO9660 whose block 0 carries the EBCDIC `IBMA` IPL record that the
# firmware's `aix-boot` package recognises.  It is 603 MB and cannot be
# committed (proposal §13 R8), so this row is FIXTURE-GATED: it fails
# loudly with the path it wanted rather than skipping silently.

TEST_NAME := ANS AIX boot
TEST_DESC := Boots the AIX 4.1.5 Install CD through Open Firmware's documented Service-keyswitch path

# 4 MB Apple Network Server 500/700 ROM, Open Firmware 1.1.22.
TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=65536

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
