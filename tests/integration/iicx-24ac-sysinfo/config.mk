# Integration test configuration: IIcx + Display Card 24AC — floppy insert +
# launch the Symantec "System Info" application.
#
# A variant of iicx-24ac: it boots the same 32-bit System 7.1 SCSI cdev image
# (systems/system_7_1_20mb_24ac_cd_32bit_gc.img) to an 8-bpp COLOUR Finder desktop
# on the IIcx + Display Card 24AC, but instead of exercising the acceleration
# engine through the Apple menu / Control Panels scroll, it inserts the
# "Utilities Disk 2" floppy, lets the Finder open its window, and double-clicks
# the "System Info" application to launch it.  System Info's benchmark window is
# the pixel-exact target — it reads back "Apple Macintosh 24AC (8 bit)" as the
# display, so it doubles as an end-to-end read-out of the 24AC card by the OS.
#
# Both disk images live in gs-test-data and are provisioned into tests/data by
# scripts/fetch-test-data.sh: the 20 MB HD as systems/…_24ac_cd_32bit.img.7z
# (auto-extracted) and the 1.44 MB floppy as apps/Norton-Utils-Disk-2.image
# (raw).  No TEST_SETUP is needed.

TEST_NAME := IIcx Display Card 24AC — floppy insert + System Info Display benchmark
TEST_DESC := Boot IIcx + 24AC to the colour Finder, insert "Utilities Disk 2", launch System Info, select only Test Display, and Run the Display benchmark (PRAM seeded so no System Differences dialog)

TEST_ROM := roms/IIcx.rom

# The harness creates the IIcx with 8 MB; the script re-boots with the 24AC
# card selected (video_card can't be passed as an arg) and attaches the SCSI HD.
TEST_ARGS := model=iicx ram=8192
