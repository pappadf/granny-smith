# Integration test configuration: IIcx + 8•24 GC — format a blank HD under System 6.0.8
#
# Boots a Macintosh IIcx with the Apple Macintosh Display Card 8•24 (JMFB,
# mdc_8_24 — loads mdc-8-24-revb-d1629664.vrom next to the ROM) from the SSW 6.0.8
# 800K boot floppy (Disk2of4, volume "Utilities"), launches Apple HD SC Setup
# v2.0.3 off that floppy, and formats a freshly-created blank HD20SC SCSI disk
# at ID 0 — all driven by mouse clicks, matched pixel-exact at each milestone.
#
# This is the regression guard for the NCR 5380 BLIND-vs-DRQ primer-gate fix
# (scsi.c write_uint8 / scsi.h SCSI_BLIND_SEL): before the fix the primer-slot
# gate ran on Mac OS's DRQ pseudo-DMA writes too and dropped the leading $00 of
# every zero-filled block, so HD SC Setup's HFS volume-init wrote only the MDB
# and one bitmap block, then aborted ("unable to mount volume"). The gate now
# applies to the BLIND window only (A/UX's CLR.B primer), so Mac OS's DRQ writes
# land intact and the format completes ("Disk initialization successfully
# completed").

TEST_NAME := IIcx Format Blank HD (8•24, System 6.0.8)
TEST_DESC := Boot IIcx + 8•24 GC from SSW 6.0.8 floppy, run Apple HD SC Setup, format a blank HD20SC

TEST_ROM := roms/iix-iicx-se30-97221136.rom

# The harness creates the IIcx with 8 MB; the script re-boots with the 8•24
# card selected (video_card can't be passed as an arg), attaches the blank HD,
# and inserts the boot floppy.
TEST_ARGS := model=iicx ram=8192
