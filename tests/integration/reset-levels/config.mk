# Integration test: the reset levels, on every model in the registry.
#
# One conformance function (test.script) run over every machine the
# emulator builds, so a family cannot land without a conforming reset:
#   level 2  machine.reset    the CPU back at its reset vector, PRAM/NVRAM kept
#   level 3  machine.restart  the same, with the RAM cold; nothing rebuilt
# and a new machine from the same document (machine.boot) gets the
# constructor's PRAM and a blank NVRAM.
# On the models whose ROM boots the Mac OS start-up within the budget, each
# level must also really reboot: Ticks comes back near its fresh-boot value
# instead of carrying on from where it was.

TEST_NAME := Reset levels (every model)
TEST_DESC := machine.reset / machine.restart conform on every model, and a new machine.boot inherits no store: vector, non-volatile stores, a real reboot

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := model=plus

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
TEST_TIMEOUT := 3600
