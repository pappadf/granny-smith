# Integration test: the RTC is a simulated-time counter, seeded from the
# host wall clock when the machine is built, and a `machine.rtc.time = N`
# written on the line after the boot overrides it and sticks.
#
#   * positive control: a pinned 68k (SE/30) and a pinned PDM clock advance by
#     simulated seconds only, never by host time;
#   * negative control: a pin does not leak into the next machine -- the new
#     machine reads the wall clock, which is what fails if anyone brings back
#     a staged seed;
#   * continuity: machine.reset and machine.restart keep the counter ticking
#     (they never rebuild the RTC); only a new machine starts it again;
#   * determinism: a pinned boot run twice gives a byte-identical frame.

TEST_NAME := RTC seeding and continuity
TEST_DESC := wall-clock by default, a pin written after the boot sticks, never leaks to the next machine, and survives reset/restart

TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_ARGS := model=se30 ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
