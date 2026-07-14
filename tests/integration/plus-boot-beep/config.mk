# Integration test: Plus boot beep — golden-WAV capture (sound stage 0)
#
# Exercises the generalized audio path end to end on the Mac Plus: the PWM
# buffer scan (sound.c) pushes int16 mono frames through audio_out, the
# deterministic capture sink records them at guest rate (22,255 Hz), and
# machine.sound.match compares the capture sample-exactly against the
# committed golden WAV — the audio analog of the screen.match PNG tests.
#
# The ROM opens and closes the sound-enable window around the beep itself
# (~42 VBLs = 15,540 frames), so any instruction budget past the beep yields
# the identical capture; the golden is bit-reproducible on every host.

TEST_NAME := Plus boot beep — golden WAV capture
TEST_DESC := machine.sound.capture/match: deterministic Plus PWM audio through the generalized audio path

TEST_ROM := roms/Plus_v3.rom
