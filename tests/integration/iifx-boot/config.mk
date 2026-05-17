# Integration test configuration: IIfx Boot
# Verifies that the IIfx ROM reaches NuBus video with OSS/IOP hardware present.

TEST_NAME := IIfx Boot
TEST_DESC := Boots Macintosh IIfx with its 512 KB ROM and verifies NuBus video initialization

TEST_ROM := roms/4147DD77-IIfx.rom

TEST_ARGS := model=iifx ram=8192
