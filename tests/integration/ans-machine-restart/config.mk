# Integration test: machine.restart on the Apple Network Servers.
#
# The power-cycle contract (nothing torn down, media kept) on the first
# machine in the repository with TWO visible SCSI buses.  A SCSI id does not
# identify a device here — `machine.scsi` is fast/wide channel 0 and
# `machine.scsi2` is channel 1 — so both channels' media must still be on
# their own bus afterwards.

TEST_NAME := ANS machine.restart
TEST_DESC := machine.restart keeps media on BOTH fast/wide channels of an ans500/ans700; the red button resets PRAM, not NVRAM

# 4 MB Apple Network Server 500/700 ROM, Open Firmware 1.1.22.
TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=32768

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
