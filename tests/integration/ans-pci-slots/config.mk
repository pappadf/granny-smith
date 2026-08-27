# Integration test: Apple Network Server PCI topology
# (proposal-apple-network-server-500-700 §5.2, §5.3, §5.7, §5.8)
#
# The Network Server's PCI delta is three of the four boot-critical items
# in Apple's own table, and all three are pure data — which is exactly why
# they need a row: nothing about a wrong IDSEL or a wrong interrupt line is
# loud.  A subtly wrong config answer yields a SILENTLY UNCONFIGURED DEVICE
# under AIX rather than a crash, which is the hardest failure shape there
# is (proposal §7).
#
# What is pinned here:
#
#   * THE SIX-DEVICE BANDIT.  Bandit 1 carries two sockets plus three
#     soldered devices plus Grand Central, with no PCI-to-PCI bridge, and
#     the slot split is 2/4 rather than the 9500's 3/3.
#   * THE INTERRUPT REWIRING.  Exactly three Grand Central external lines
#     move from the 9500's assignment, and slot 3 sits on Bandit 2 while
#     keeping EXT5 — a line derived from its bridge would be wrong for that
#     one slot and right for the other five.
#   * MACHINE IDENTITY.  The 53C825A's Revision ID must have bit 4 set or
#     the ROM's own `825a?` probe fails, `?esb` never flips, and the
#     machine is not a Network Server.
#   * THE 54M30 HAS NO INTERRUPT LINE, and must not be allocated one.

TEST_NAME := ANS PCI slots
TEST_DESC := Slot topology, IDSELs, the rewired interrupt map and the built-in device identities on ans500/ans700

# 4 MB Apple Network Server 500/700 ROM, Open Firmware 1.1.22.
TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=32768

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
