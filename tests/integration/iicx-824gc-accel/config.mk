# Integration test configuration: IIcx + Display Card 8•24 GC — accelerator ON
#
# Boots the IIcx with the HLE 8•24 GC card ("Dolphin") and 8 MB RAM from a
# System 6.0.8 SCSI HD image whose accelerator support file is named exactly
# "8•24 GC" (systems/system_6_0_8_20mb_8_24gc.img).  On that image the
# Start-Manager ROM patch (PTCH 376 / Load8Dot24GC) runs the gc24-32 installer
# at boot, which installs + opens + configures the real .GraphAccel driver and
# turns the accelerator ON — the full csCode/CB bring-up path through GCQD.
#
# This is strictly stronger than iicx-824gc (which fakes the host side by
# poking the CB): here the *real* driver + GCQD bring the accelerator up
# (gc.on, gc.error==0, RPCs serviced) and the Finder desktop renders correctly
# with no "having difficulty" dialog.
#
# The image is stored in gs-test-data as
# systems/system_6_0_8_20mb_8_24gc.img.7z and auto-extracted by
# scripts/fetch-test-data.sh.

TEST_NAME := IIcx Display Card 8•24 GC — accelerator ON (real .GraphAccel bring-up)
TEST_DESC := Boot IIcx + 8•24 GC with the accelerator enabled; bring-up clean (gc.on, error 0, RPCs), desktop renders, no dialog

TEST_ROM := roms/iix-iicx-se30-97221136.rom

# The harness creates the IIcx with 8 MB; the script re-boots with the 8•24 GC
# card selected (video_card can't be passed as an arg) and attaches the SCSI HD.
TEST_ARGS := model=iicx ram=8192
