# Integration test configuration: IIcx Boot
# Verifies that the IIcx boots the Universal ROM far enough to identify
# itself as Macintosh IIcx via machine-ID bits.

TEST_NAME := IIcx Boot
TEST_DESC := Boots IIcx with Universal ROM and verifies machine-ID identification

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/IIcx.rom

# `model=iicx` selects the IIcx profile from the Universal ROM's
# compatibility list (the same ROM defaults to SE/30 otherwise).
# 8 MB RAM gives the ROM's RAM tests room to complete within the cycle
# budget without dragging the test out.
TEST_ARGS := model=iicx ram=8192
