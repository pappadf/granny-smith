# Integration test: capture the Lisa's Serial A output to a host file
#
# Cold-boots Lisa Office System 3.1 from the installed ProFile (as the
# suite-lisa row does), attaches a host file to Serial A
# (`machine.scc.a.output`), opens the Calculator from the Disk, shows its
# tape, selects it and chooses File/Print "Print Current Tape Selection".
# The Office System's device configuration puts an "Imagewriter / II DMP"
# on Serial A, so its RS-232 driver opens SCC channel A, waits for DSR (the
# ready line an attached output raises, on /SYNC) and streams the page,
# which lands in the file byte for byte.  Then the output is detached and
# the same print must fail with the driver's "difficulty printing" state
# (no bytes anywhere).
#
# The file is kept in the results directory as serial-a.bin: the raw
# ImageWriter command stream LOS sent.
#
# What it guards: the empty-slot bus error (the OS's device configuration),
# the SCC's full $D000-$D3FF decode, the SCC input pins and the ready-line
# wiring, the SCC's WR1-gated INT line and Tx-pending latch, and the
# channel output itself -- without any one of them the driver never sends,
# or the machine crashes on the first byte.

TEST_NAME := Lisa Serial A output capture (LOS print)
TEST_DESC := LOS 3.1 prints the Calculator tape through Serial A into machine.scc.a.output; the file holds the ImageWriter stream.

TEST_ROM := roms/lisa2-revh-098917b2.rom

# A private copy of the installed ProFile: the Office System writes to it.
TEST_SETUP := cp "$(TEST_DATA)/Lisa/LisaOfficeSystem-3.1/LOS-3.1-ProFile.image" "$(WORK_DIR)/profile.image"

TEST_ARGS := model=lisa ram=2048

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
