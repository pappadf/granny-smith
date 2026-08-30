# Integration test: machine.restart on the Apple Network Servers
# (proposal-apple-network-server-500-700 §8 Phase F, §9).
#
# The power-cycle contract (proposal-boot-vs-reset §3.2/§3.3) meets the
# first machine in the repository with TWO visible SCSI buses.  A SCSI id
# does not identify a device here — `machine.scsi` is fast/wide channel 0
# and `machine.scsi2` is channel 1 — so the medium's BUS has to survive the
# teardown along with its handle.  A transfer that only walks channel 0
# loses channel 1's drives silently: the handle stays on the tracked-image
# list, system_destroy closes it, and the drive is simply gone, taking
# every delta write in it.

TEST_NAME := ANS machine.restart power-cycle
TEST_DESC := machine.restart rebuilds an ans500/ans700 and keeps media on BOTH fast/wide channels attached

# 4 MB Apple Network Server 500/700 ROM, Open Firmware 1.1.22.
TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=32768

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
