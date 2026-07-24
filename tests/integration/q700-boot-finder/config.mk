# Integration test: Quadra 700 boots System 7.1 to the Finder (Phase D gate)
#
# Disk Tools floppy boot on the q700 profile: the DAFB/Swatch/AC842/DP8531
# pipeline carries the ROM's 640x480 mode set (sense code 6 via the
# tristate-reset drive register), the System's driver re-programs the mode
# (66.9 Hz Apple timing from the programmed DP8531 + Swatch values), and
# the Finder desktop renders pixel-exactly against the committed golden.

TEST_NAME := Quadra 700 System 7.1 floppy boot to Finder
TEST_DESC := Disk Tools boot: DAFB 640x480 desktop pixel-match

TEST_ROM := roms/q700-q900-420dbff3.rom
TEST_ARGS := model=q700 ram=8192
