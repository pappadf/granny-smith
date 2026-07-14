# Integration test: IIfx boot chime — golden-WAV capture (sound stage 3)
#
# Exercises the ASC producer on the OSS machine. Doubles as the empirical
# half of the proposal's IIfx sound-identity question (§7): the ROM's chime
# and its ASC POST (iifx-mactest) both run against the standard discrete
# ASC (ascVersion = $00) and take the standard code paths — the internal
# Feb-91 roadmap's "Batman/EASC + DFAC" identity is not required for
# playback. The ASC IRQ routes to OSS source 8 (OSSIntSound, status $202,
# level register $008); the wavetable chime itself raises no IRQs.

TEST_NAME := IIfx boot chime — golden WAV capture
TEST_DESC := machine.sound.capture/match: ASC wavetable chime on the OSS machine (channel-A mix)

TEST_ROM := roms/4147DD77-IIfx.rom
TEST_ARGS := model=iifx ram=8192
