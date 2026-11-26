# Integration test: the recorder's sound is the guest's, on the guest's time.
#
# plus-musicworks' session -- the Plus boots the MusicWorks 0.42 floppy,
# opens Brandenburg No. 3 and plays it -- recorded from the first frame.
# The capture window plus-musicworks golden-matches (music.wav, taken before
# the guest's volume) appears sample for sample in the recording's decoded
# FLAC, at the place emulated time puts it: within the timeline's window of
# the window's start time.  See docs/internals/core/record/record.md.

TEST_NAME := Recorder — MusicWorks sound against the plus-musicworks golden
TEST_DESC := Record the MusicWorks session; the golden music window is found exactly in the decoded FLAC, at its emulated time

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := fd=$(TEST_DATA)/apps/MusicWorks-0.42.image --record=$(WORK_DIR)/musicworks.mkv

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
