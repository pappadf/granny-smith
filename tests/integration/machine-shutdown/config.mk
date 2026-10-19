# Integration test: machine.shutdown — switch the machine off for good.
#
# A Plus with a floppy in its drive is shut down: no machine is left
# (machine.created false, the hardware tree gone, unplugged from the
# AppleTalk cable), a second shutdown is refused, and machine.boot builds
# the next machine as from a fresh start.

TEST_NAME := machine.shutdown teardown (Plus)
TEST_DESC := machine.shutdown leaves no machine: created false, hardware gone, network unplugged; the next machine.boot starts clean

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := model=plus

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
