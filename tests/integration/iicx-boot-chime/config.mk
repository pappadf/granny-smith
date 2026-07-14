# Integration test: IIcx boot chime — golden-WAV capture (sound stage 1)
#
# Same producer path as se30-boot-chime but with the IIcx board mix: the
# internal speaker takes the ASC's left channel (voices 0+1) rather than the
# SE/30's two-channel sum, so this golden differs from the SE/30's.

TEST_NAME := IIcx boot chime — golden WAV capture
TEST_DESC := machine.sound.capture/match: ASC wavetable chime through the producer path (IIcx channel-A mix)

TEST_ROM := roms/IIcx.rom
TEST_ARGS := model=iicx
