# Integration test configuration: IIcx + 8•24 GC — Control Panel volume beep
#
# Boots the IIcx with the HLE 8•24 GC card and 8 MB RAM from the System 6.0.8
# SCSI HD image (systems/system_6_0_8_20mb_8_24gc.img), opens the Control
# Panel from the Apple menu, drags the Speaker Volume slider, and captures the
# confirmation beep the Sound Manager plays through the ASC wavetable path.
#
# Regression guard for the ASC wavetable sample-signedness bug: wavetable RAM
# bytes are offset-binary (0x80 = zero, same DAC convention as FIFO mode), but
# the producer used a plain int8 cast, wrapping every sample >= 0x80 — the
# beep's 0x80-centered sine came out grossly distorted.  The boot-chime
# goldens can't catch this (the ROM chime tables never cross 0x80, so the two
# interpretations differ only by inaudible DC); the Sound Manager beep is the
# discriminating waveform.  The beep also exercises the in-place wavetable
# envelope rewrites (~14 full table re-renders during one beep).
#
# The image is stored in gs-test-data as
# systems/system_6_0_8_20mb_8_24gc.img.7z and auto-extracted by
# scripts/fetch-test-data.sh.

TEST_NAME := IIcx 8•24 GC — Control Panel volume beep (ASC wavetable path)
TEST_DESC := Boot to Finder, open Control Panel via Apple menu, drag Speaker Volume; golden-WAV match of the Sound Manager beep

TEST_ROM := roms/iix-iicx-se30-97221136.rom

# The harness creates the IIcx with 8 MB; the script re-boots with the 8•24 GC
# card selected (video_card can't be passed as an arg) and attaches the SCSI HD.
TEST_ARGS := model=iicx ram=8192
