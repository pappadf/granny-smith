# Integration test: SE/30 boot chime — golden-WAV capture (sound stage 1)
#
# Exercises the ASC producer end to end: the ROM plays the boot chime in
# wavetable mode (four free-running voices), the drain event renders it at
# 22,257 Hz, the SE/30 board mix sums both channels to the speaker, and the
# deterministic capture sink records the frames at guest rate for a
# sample-exact match against the committed golden WAV.
#
# The ROM opens and closes the chime window itself (mode 2 → mode 0), so any
# instruction budget past the chime yields the identical capture.

TEST_NAME := SE/30 boot chime — golden WAV capture
TEST_DESC := machine.sound.capture/match: ASC wavetable chime through the producer path (SE/30 sum mix)

TEST_ROM := roms/SE30.rom
TEST_ARGS := model=se30
