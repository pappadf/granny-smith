# Integration test: suspending a recording and resuming it.
#
# recorder.suspend finalises the file being written -- the page does this
# when it is hidden or closed, so the file is whole if the page never comes
# back -- and records nothing until recorder.resume, which continues in the
# next segment (name-2.mkv).  Coded on the recorder thread and inline, the
# files are the same bytes.  See docs/internals/core/record/record.md.

TEST_NAME := Recorder — suspend finalises the file, resume opens the next segment
TEST_DESC := suspend closes a whole file and stops recording; resume continues in name-2.mkv; threaded and inline agree

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := model=plus ram=1024

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
