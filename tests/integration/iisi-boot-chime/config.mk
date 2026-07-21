# Integration test: IIsi boot chime — golden-WAV capture (sound stage 2)
#
# Same producer path as iici-boot-chime on the V8 variant: the IIsi ROM's
# chime window differs from the IIci's (13,099 frames), so this golden is
# its own fixture.

TEST_NAME := IIsi boot chime — golden WAV capture
TEST_DESC := machine.sound.capture/match: ASC wavetable chime on the V8 machine (channel-A mix)

TEST_ROM := roms/iisi-36b7fb6c.rom
TEST_ARGS := model=iisi ram=17408
