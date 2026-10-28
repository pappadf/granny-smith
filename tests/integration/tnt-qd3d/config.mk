# Integration test: the Apple QuickDraw 3D Accelerator Card (pci_card
# "qd3d") in a Power Macintosh 8500 booting Mac OS 7.6 with QuickDraw 3D
# 1.6, the card's driver and RAVE plug-in, and its test applications.

TEST_NAME := TNT QuickDraw 3D Accelerator
TEST_DESC := pm8500 + System 7.6 + QuickDraw 3D 1.6 + the QD3D Accelerator Card: driver bring-up, RaveEngineInfo, Gerbils, TextureEyes, 16-bpp output

# 4 MB Power Macintosh 7500/8500/9500 ROM v1 (stored checksum 0x96CD923D)
TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

TEST_ARGS := model=pm8500 ram=65536

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
