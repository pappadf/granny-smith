# Integration test: cold-booting an INSTALLED AIX 4.1.5 on the Apple Network
# Server (proposal-apple-network-server-500-700 §6, ladder rung S13).
#
# The media is a disk image the BOS install itself produced — AIX 4.1.5
# installed from the Install CD onto a Quantum LP240S (234 MB, the smallest
# catalog drive the stock layout fits), shut down with `shutdown -F` and
# exported flattened with `machine.scsi.device[2].image.export`.  It is
# copyrighted AIX and cannot be committed (proposal §13 R8), so this row is
# media-gated like ans-aix-boot: it skips cleanly when the image is absent.
#
# The boot path is the machine's own: blank non-volatile store, key in
# Normal, disk in bay 2 — Open Firmware's default boot device is
# `disk2:aix`, and it finds the `IBMA` boot record there without being
# told.  No CD, no typed command.

TEST_NAME := ANS AIX installed boot
TEST_DESC := Cold-boots an installed AIX 4.1.5 disk through Open Firmware's default disk2:aix to the login prompt

# 4 MB Apple Network Server 500/700 ROM, Open Firmware 1.1.22.
TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=65536

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
