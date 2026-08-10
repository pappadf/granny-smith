# Integration test: machine.restart — power-cycle the running machine
# (proposal-boot-vs-reset §3.2/§3.3).  Boots a IIcx with an 8•24 GC card and
# a System 6.0.8 hard disk to the Finder, inserts a floppy, restarts, and
# asserts the same machine came back with BOTH media still attached — as the
# SAME open storage instances (identical instance stems), which pins the
# write-durability contract: the delta survives the power-cycle, so nothing
# the guest wrote is lost.  A second boot to the Finder proves the
# transferred handles actually serve I/O.

TEST_NAME := machine.restart power-cycle (IIcx)
TEST_DESC := machine.restart rebuilds the recorded machine and keeps mounted media attached

TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_ARGS := model=iicx ram=8192

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
