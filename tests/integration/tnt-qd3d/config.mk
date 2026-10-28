# Integration test: the QuickDraw 3D Accelerator Card test rig — a
# Power Macintosh 8500 booting Mac OS 7.6 with QuickDraw 3D 1.6, the
# card's driver and RAVE plug-in, and its test applications.
#
# WORK IN PROGRESS.  There is no device model for the card yet, so the
# test skips unless TEST_VARS="QD3D_WIP=1" is given; see test.script.

TEST_NAME := TNT QuickDraw 3D Accelerator
TEST_DESC := pm8500 + System 7.6 + QuickDraw 3D 1.6: boot, Gerbils and RaveEngineInfo on the software engine (card milestones pending a device model)

# 4 MB Power Macintosh 7500/8500/9500 ROM v1 (stored checksum 0x96CD923D)
TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

TEST_ARGS := model=pm8500 ram=65536

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
