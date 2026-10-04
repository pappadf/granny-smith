# Integration test: print from the Lisa Office System to the virtual ImageWriter
#
# Cold-boots Lisa Office System 3.1 from the installed ProFile (as
# lisa-serial-output does), plugs the virtual ImageWriter II into Serial A
# (`machine.imagewriter.connection = "serial-a"`), opens the Calculator,
# shows and selects its tape and prints it.  The Office System's driver
# opens SCC channel A, waits for DSR -- the printer's DTR, which it raises
# when plugged in and selected -- and streams the page; the printer
# interprets it, and when the input has been quiet for the idle timeout the
# job ends and its PDF lands in the results directory (--print-dir).
#
# What it guards: the SCC port-device seam (bytes to the device, its ready
# line on /SYNC), the interpreter on a real driver's stream, the job's end
# on idle, and the headless printer sink.  The PDF's CRC is the golden: the
# output is deterministic.

TEST_NAME := Lisa prints to the virtual ImageWriter
TEST_DESC := LOS 3.1 prints the Calculator tape on Serial A to machine.imagewriter; one job, one page, a PDF in --print-dir.

TEST_ROM := roms/lisa2-revh-098917b2.rom

# A private copy of the installed ProFile: the Office System writes to it.
TEST_SETUP := cp "$(TEST_DATA)/Lisa/LisaOfficeSystem-3.1/LOS-3.1-ProFile.image" "$(WORK_DIR)/profile.image"

TEST_ARGS := model=lisa ram=2048 --print-dir=$(TEST_RESULTS_DIR)

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
