# Integration test: print from the Lisa Office System on Serial A, to a
# host file and to the virtual ImageWriter
#
# Cold-boots Lisa Office System 3.1 from the installed ProFile (as the
# suite-lisa row does), opens the Calculator from the Disk, shows its tape,
# selects it and chooses File/Print "Print Current Tape Selection" twice.
# The Office System's device configuration puts an "Imagewriter / II DMP"
# on Serial A, so its RS-232 driver opens SCC channel A, waits for DSR and
# streams the page.
#
# First print: a host file on Serial A (`machine.scc.a.output`), whose
# ready line is /SYNC.  The page lands in the file byte for byte (kept in
# the results directory as serial-a.bin, the raw ImageWriter command
# stream).  The output is then detached.
#
# Second print: the virtual ImageWriter II plugged into Serial A
# (`machine.imagewriter.connection = "serial-a"`), whose DTR is the DSR the
# driver waits for.  The printer interprets the stream, and when the input
# has been quiet for the idle timeout the job ends and its PDF lands in the
# results directory (--print-dir).  The PDF's CRC is the golden: the output
# is deterministic.  Deselected, the printer drops DTR and the same print
# cannot go out.
#
# What it guards: the empty-slot bus error (the OS's device configuration),
# the SCC's full $D000-$D3FF decode, the SCC input pins and the ready-line
# wiring, the SCC's WR1-gated INT line and Tx-pending latch, the channel
# output itself, the SCC port-device seam (bytes to the device, its ready
# line on /SYNC), the interpreter on a real driver's stream, the job's end
# on idle, and the headless printer sink.  (This was two tests,
# lisa-serial-output and lisa-imagewriter, each booting the Office System
# for one print.)

TEST_NAME := Lisa prints on Serial A (file, ImageWriter)
TEST_DESC := LOS 3.1 prints the Calculator tape on Serial A into machine.scc.a.output, then to machine.imagewriter; one job, one page, a PDF in --print-dir.

TEST_ROM := roms/lisa2-revh-098917b2.rom

# A private copy of the installed ProFile: the Office System writes to it.
TEST_SETUP := cp "$(TEST_DATA)/Lisa/LisaOfficeSystem-3.1/LOS-3.1-ProFile.image" "$(WORK_DIR)/profile.image"

TEST_ARGS := model=lisa ram=2048 --print-dir=$(TEST_RESULTS_DIR)

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
