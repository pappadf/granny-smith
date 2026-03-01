# Integration test configuration: SE/30 Boot
# Verifies that the SE/30 machine loads the Universal ROM and boots without crashing

TEST_NAME := SE/30 Boot
TEST_DESC := Boots SE/30 with Universal ROM for 50M instructions, verifies no bus errors or crashes

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/IIcx.rom

# No extra disk images; just verify ROM boot sequence
TEST_ARGS :=
