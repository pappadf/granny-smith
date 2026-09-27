# Integration test: the Lisa's on-board ImageWriter on Serial A
#
# Cold-boots Lisa Office System 3.1 from the installed ProFile (as the
# suite-lisa row does), opens the Calculator from the Disk, shows its tape,
# selects it and chooses File/Print "Print Current Tape Selection".  The
# Office System's device configuration puts an "Imagewriter / II DMP" on
# Serial A, so the OS's RS-232 driver opens SCC channel A, waits for DSR
# (the printer's ready line, on /SYNC) and streams the page to the printer
# model, which ends the job after 5 s of guest time with no data.
#
# The assertions read `machine.printer` (docs/core/peripherals/imagewriter.md):
# one job, of the size and shape the Office System driver sends.  The job
# itself is written to the results directory (--print-dir) as iw-00001.iw.
#
# What it guards: the empty-slot bus error (the OS's device configuration),
# the SCC's full $D000-$D3FF decode, the SCC input pins, and the SCC's
# WR1-gated INT line and Tx-pending latch -- without any one of them the
# driver never sends, or the machine crashes on the first byte.

TEST_NAME := Lisa ImageWriter print (Serial A)
TEST_DESC := LOS 3.1 prints the Calculator tape to the on-board ImageWriter; one job of ImageWriter graphics arrives.

TEST_ROM := roms/lisa2-revh-098917b2.rom

# A private copy of the installed ProFile: the Office System writes to it.
TEST_SETUP := cp "$(TEST_DATA)/Lisa/LisaOfficeSystem-3.1/LOS-3.1-ProFile.image" "$(WORK_DIR)/profile.image"

TEST_ARGS := model=lisa ram=2048 --print-dir=$(TEST_RESULTS_DIR)

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
