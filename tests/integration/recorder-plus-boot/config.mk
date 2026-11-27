# Integration test: the screen recorder on a Plus boot, headless.
#
# --record starts the recording before the machine's first frame; the
# script pins the clock, boots ~25 s of emulated time, and stops.  The file
# is a pure function of the guest's execution (headless blocks on a full
# ring), so its CRC-32 is a golden; its last frame, decoded in tree, is the
# screen at the same point.  The same boot recorded again with the coder
# inline (recorder.threaded = false, as --recorder=inline) gives the same
# bytes.  The beep check records raw (recorder.ac_coupled = false), so the
# capture is in the file sample for sample.  See
# docs/internals/core/record/record.md.

TEST_NAME := Recorder — Plus boot (golden file, inline = threaded)
TEST_DESC := --record a Plus boot; the file's CRC is a golden, the last decoded frame is screen.save, inline coding is byte-identical

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := model=plus ram=1024 --record=$(WORK_DIR)/boot.mkv

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
