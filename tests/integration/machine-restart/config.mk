# Integration test: machine.restart — power-cycle the running machine — and
# machine.rebuild — construct the recorded machine again.  Boots a IIcx with
# an 8•24 GC card and a System 6.0.8 hard disk to the Finder and inserts a
# floppy.  machine.restart must tear nothing down: the media, PRAM and RTC are
# the same devices afterwards, RAM is cold, and the guest boots again.
# machine.rebuild must bring the same machine back with BOTH media still
# attached — as the SAME open storage instances (identical instance stems),
# which pins the write-durability contract: the delta survives the teardown,
# so nothing the guest wrote is lost.  A boot to the Finder after each proves
# the media actually serve I/O.

TEST_NAME := machine.restart power-cycle and machine.rebuild (IIcx)
TEST_DESC := machine.restart power-cycles without a teardown; machine.rebuild reconstructs the recorded machine and keeps mounted media attached

TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_ARGS := model=iicx ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
