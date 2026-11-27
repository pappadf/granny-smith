# Integration test: the recorder's sound is AC-coupled, as it leaves the machine.
#
# A Plus at the floppy question mark, its PWM buffer then left at a level
# far from 128 (as a game leaves it idle): the recorded sound steps when the
# level moves and decays to silence, carries no DC, and joins across a
# suspend and resume without a step -- the recorder's DC blocker runs on
# while suspended.  The same session with recorder.ac_coupled = false
# records the DAC's raw level; coded on the recorder thread and inline,
# the files are the same bytes.  See docs/internals/core/record/record.md.

TEST_NAME := Recorder — AC-coupled sound: no DC, no step at a suspend/resume join
TEST_DESC := A Plus's PWM buffer left at an offset: the coupled recording decays to 0 and joins its segments without a step; raw keeps the level

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := model=plus ram=1024

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
