# Integration test: IIci boot chime — golden-WAV capture (sound stage 2)
#
# Exercises the ASC producer on an RBV machine: the IIci ROM plays its boot
# chime in wavetable mode, the drain renders it at 22,257 Hz, the internal-
# speaker mix takes channel A, and the deterministic capture sink records the
# frames at guest rate for a sample-exact match against the committed golden.
# (The RBV sound-IRQ wiring itself — RvSndIRQ flag/enable — is pinned by the
# tests/unit/suites/rbv unit suite; the wavetable chime does not raise IRQs.)

TEST_NAME := IIci boot chime — golden WAV capture
TEST_DESC := machine.sound.capture/match: ASC wavetable chime on the RBV machine (channel-A mix)

TEST_ROM := roms/iici-368cadfe.rom
TEST_ARGS := model=iici ram=8192
