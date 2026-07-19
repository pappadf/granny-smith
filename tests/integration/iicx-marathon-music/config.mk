# Integration test: IIcx + 8•24 GC — Marathon v1.2 in-game background MUSIC.
#
# Distinct from the beep/chime tests (short Sound-Manager wavetable bursts):
# this targets the QuickTime™ Musical Instruments MIDI music that plays during
# live gameplay.  Per the game's own behaviour, the level song starts once the
# first-person gameplay screen is up (the "Arrival" cut-scene hands off to the
# HUD view — marathon-game-8bpp-640x480.png).  The test drives the standard
# launch -> Preferences(Background Music ON) -> Begin New Game flow, runs until
# that gameplay screen is on-screen, then captures the ASC output and asserts
# REAL music signal is present (DC-compensated capture.peak well above the
# offset-binary silence floor).
#
# NOTE: as of this test's creation the assertion FAILS — in-game MIDI music
# does not play in the emulator (the ASC runs but streams silence during
# gameplay; the Background Music setting produces no audible difference).  This
# is independent of the 8•24 GC / IIfx work (verified silent at clean HEAD too).
# The test is the executable spec for the fix: it goes green when the
# QuickTime-music path produces audio.  See tmp/marathon-music-findings.md.

TEST_NAME := IIcx 8•24 GC — Marathon in-game background music (QuickTime MIDI)
TEST_DESC := Run to the Marathon gameplay screen, then assert QuickTime background music is audible (currently RED — documents the missing-music bug)

TEST_ROM := roms/IIcx.rom
TEST_ARGS := model=iicx ram=8192
