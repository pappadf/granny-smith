# Integration test configuration: IIx Boot
# Verifies that the IIx boots the Universal ROM far enough to identify
# itself as Macintosh IIx via machine-ID bits (PA6 = 0, PB3 = 0).

TEST_NAME := IIx Boot
TEST_DESC := Boots IIx with Universal ROM and verifies machine-ID identification

TEST_ROM := roms/IIcx.rom

TEST_ARGS := model=iix ram=8192
