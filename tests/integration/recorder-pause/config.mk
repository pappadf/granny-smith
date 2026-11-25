# Integration test: the recorder follows the guest's clock.
#
# A recording counts frame-units, not host time: runs of odd sizes (frames
# cut short and resumed), with the machine stopped and the host waiting in
# between, give exactly one output frame per completed frame-unit at full
# rate, one per two at half, one per four at quarter -- and the files hold
# exactly those frames at 1203/20, 1203/40 and 1203/80 fps.  See
# docs/internals/core/record/record.md.

TEST_NAME := Recorder — pause, odd runs and the three rates
TEST_DESC := frames == completed frame-units / divisor exactly at full, half and quarter rate, across stops and host waits

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := model=plus ram=1024

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
