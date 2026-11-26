# Integration test: a recording across a new machine.
#
# checkpoint.load of a machine that fits the recording (same screen, same
# sound stream) is a cut in the same file, coded as a keyframe; machine.boot
# is always the next segment, even of the same model; the frame-unit count
# carries on across both; shutting the machine down finalises the file.
# See docs/internals/core/record/record.md.

TEST_NAME := Recorder — checkpoint load is a cut, boot is a new segment
TEST_DESC := checkpoint.load continues the file with a keyframe; machine.boot starts name-2.mkv; a shut-down finalises

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := model=plus ram=1024

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
