# Integration test configuration: IIcx + Display Card 8•24 GC — System 7.1 +
# the Symantec "System Info" Display benchmark, accelerator ON.
#
# The System-7.1 sibling of iicx-24ac-sysinfo, and the 8•24 GC's ONLY System 7
# coverage: it boots systems/system_7_1_20mb_24ac_cd_32bit_gc.img (the 24AC
# cdev image + the "8•24 GC" System-7 control panel installed, 32-bit
# addressing on) on the IIcx + 8•24 GC.  Under System 7 every window port is a
# CGrafPort, GCQD flushes with func $26 before every CopyBits, and ScrollRect
# runs screen->screen func $15 blits — the three paths whose regressions were
# invisible under the System 6 tests (commits 85d0b97 / 7828410 / f2264c5).
# This scenario pins all three: it inserts "Utilities Disk 2", launches System
# Info, selects only "Test Display", and runs the Display benchmark to its
# System Ratings result — every scene pixel-exact with the accelerator drawing.
#
# Both fixtures live in gs-test-data and are provisioned into tests/data by
# scripts/fetch-test-data.sh: the HD image as
# systems/system_7_1_20mb_24ac_cd_32bit_gc.img.7z (auto-extracted) and the
# floppy as apps/Norton-Utils-Disk-2.image (raw).  The same HD image also
# serves the iicx-24ac* tests (verified a pixel-exact drop-in for the plain
# 24AC image, which it supersedes on this branch).

TEST_NAME := IIcx Display Card 8•24 GC — System 7.1 System Info Display benchmark (accel ON)
TEST_DESC := Boot IIcx + 8•24 GC to the System 7.1 colour Finder, insert "Utilities Disk 2", launch System Info, select only Test Display, and Run the Display benchmark — accelerator on, pixel-exact

TEST_ROM := roms/iix-iicx-se30-97221136.rom

# The harness creates the IIcx with 8 MB; the script re-boots with the 8•24 GC
# card selected (video_card can't be passed as an arg) and attaches the SCSI HD.
TEST_ARGS := model=iicx ram=8192
