# Integration test: machine.restart — power-cycle the running machine.  Boots
# a IIcx with an 8•24 GC card and a System 6.0.8 hard disk to the Finder and
# inserts a floppy.  machine.restart must tear nothing down: the media, PRAM
# and RTC are the same devices afterwards, RAM is cold, and the guest boots
# again off the same disk.  A checkpoint restore then brings the saved
# machine back with its medium.

TEST_NAME := machine.restart power-cycle (IIcx)
TEST_DESC := machine.restart power-cycles without a teardown: media, PRAM and RTC kept, RAM cold, the guest boots again

TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_ARGS := model=iicx ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
