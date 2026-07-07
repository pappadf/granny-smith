# Integration test configuration: IIcx + Display Card 8•24 GC — boot to desktop
#
# Boots the IIcx with the HLE 8•24 GC card ("Dolphin") and 8 MB RAM from the
# 20 MB System 6.0.8 SCSI HD image that has the 8•24 GC drivers installed
# (systems/system_6_0_8_20mb_8_24gc.img).  Reaches the B&W (1-bpp) 640×480
# Finder desktop and matches it pixel-exact — the same desktop the plain
# mdc_8_24 card produces.
#
# This complements iicx-824gc (which drives the accelerator command-block
# bring-up directly): here the genuine decl-ROM video driver brings the display
# up end-to-end through a real OS boot.
#
# The image is stored in gs-test-data as systems/system_6_0_8_20mb_8_24gc.img
# and auto-extracted by scripts/fetch-test-data.sh.

TEST_NAME := IIcx Display Card 8•24 GC — boot System 6.0.8 to the Finder desktop
TEST_DESC := Boot IIcx + 8•24 GC from the System 6.0.8 SCSI HD image to a 640×480 1-bpp Finder desktop

TEST_ROM := roms/IIcx.rom

# The harness creates the IIcx with 8 MB; the script re-boots with the 8•24 GC
# card selected (video_card can't be passed as an arg) and attaches the SCSI HD.
TEST_ARGS := model=iicx ram=8192
