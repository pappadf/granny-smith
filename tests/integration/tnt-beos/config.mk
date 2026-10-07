# Integration test: BeOS 5.0.3 on the Power Macintosh 9500 and 9500/180MP
#
# Mac OS 7.6 boots from MESH id 0, BeOS_Launcher (placed in the System
# Folder's Startup Items) hands the machine over, and BeOS boots from the
# installed BFS disk at MESH id 1 to the Tracker desktop — on one 604, and
# on the dual-processor card with both processors running the kernel.
#
# THE MEDIA IS IN gs-test-data: systems/system_7_6_170mb_beos_launcher.img.7z
# (the suite-tnt 7.6 image plus BeOS_Launcher from the BeOS 5.0.3 Professional
# disc in Startup Items) and systems/beos_5_0_3_ppc_500mb.img.7z (a 500 MB
# SCSI disk BeOS 5.0.3's own Installer initialised and filled on this
# machine, booted once).  At an older pin the rows SKIP.

TEST_NAME := BeOS on the 9500 and 9500/180MP
TEST_DESC := Mac OS 7.6 hands over to BeOS 5.0.3 through BeOS_Launcher; BeOS boots to the Tracker desktop on a 9500 and, with both processors, on a 9500/180MP

TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

TEST_ARGS := model=pm9500 ram=65536

# Two cold boots through Mac OS into BeOS; the MP row interprets two cores.
TEST_TIMEOUT := 1800

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
