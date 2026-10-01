# Integration test: the beige G3's PCI slot topology and config-cycle
# contract, and a mach64 GX card in socket A1 sized and assigned by the
# Rev C ROM's Open Firmware.

TEST_NAME := Gossamer PCI slots
TEST_DESC := Slot topology, empty-socket semantics and a seated mach64 GX on the beige G3's Grackle bus

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

TEST_ARGS := model=pmg3dt ram=65536

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
