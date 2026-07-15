# Integration test configuration: Plus + MusicWorks 0.42 — play music
#
# Boots the Macintosh Plus from the MusicWorks 0.42 400K MFS floppy
# (DiskCopy 4.2 image with tag bytes), navigates the Finder to
# Samples/"Brandenburg #3", opens it in MusicWorks, clicks PLAY, and captures
# a golden-WAV window of the four-voice music through the Plus PWM sound
# buffer — a long-form, application-driven workout of the generalized audio
# path (plus-boot-beep covers only the ROM's fixed 42-VBL beep window).
#
# The floppy image lives in gs-test-data as apps/MusicWorks-0.42.image and is
# fetched by scripts/fetch-test-data.sh.

TEST_NAME := Plus MusicWorks — Brandenburg No. 3 playback
TEST_DESC := Boot MFS floppy to Finder, open Samples/Brandenburg No. 3 in MusicWorks, PLAY; golden-WAV match of the PWM music

TEST_ROM := roms/Plus_v3.rom
TEST_ARGS := fd=$(TEST_DATA)/apps/MusicWorks-0.42.image
