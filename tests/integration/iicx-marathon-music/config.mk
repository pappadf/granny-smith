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
# NOTE: this test was RED from creation until 2026-07-20.  Root cause (full
# dossier: local/gs-docs/marathon/README.md): at song start QuickTime's
# software synth needs ~380 KB of System 6 system heap (128 KB mixer LUT +
# instruments); with the image's stock 128 KB boot-block heap floor the
# instrument fetch failed with a silently swallowed memFullErr — the whole
# song "played" with zero loaded instruments (deterministic silence, noErr
# everywhere).  Fixed by growing the image's system heap to 1 MB via the
# boot blocks (bbSysHeapSize, HFS-partition offset $86 — the period-
# authentic BootMan-style fix); the image in gs-test-data carries the patch.
# The emulator was never at fault (core + timing chain fully exonerated).

TEST_NAME := IIcx 8•24 GC — Marathon in-game background music (QuickTime MIDI)
TEST_DESC := Run through the Marathon gameplay hand-off and assert the QuickTime background music is audible

TEST_ROM := roms/IIcx.rom
TEST_ARGS := model=iicx ram=8192
