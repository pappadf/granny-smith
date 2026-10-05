# Integration test: a Macintosh Plus prints to the virtual ImageWriter
#
# Boots System 6.0.8 on a Plus with the virtual ImageWriter II plugged into
# the printer port (`machine.imagewriter.connection = "serial-b"`), chooses
# the ImageWriter driver in the Chooser, and prints the Finder's Print
# Directory three times: in Faster quality (72 dpi bitmap bands), in Best (a
# 160 dpi bitmap printed in two passes 1/144 in apart) and in Draft (text in
# the printer's own draft font).  The driver first
# asks the printer who it is (ESC ?) and gets the ImageWriter II's answer;
# each document lands in the results directory (--print-dir) and its PDF's
# CRC is the golden.
#
# The Best job is deselected at the printer's front panel while it runs:
# the job waits with its input buffered, and finishes -- with the same
# document -- once the printer is selected again.
#
# What it guards: the SCC port-device seam on the Mac's channel B, the
# printer's DTR on HSKi/CTS (a wrong polarity stops the driver dead with
# "The Printer is not responding"), the ESC ? reply, the interpreter on the
# Mac driver's streams, and pausing a job.

TEST_NAME := Mac Plus prints to the virtual ImageWriter
TEST_DESC := System 6.0.8 Finder Print Directory to machine.imagewriter on the printer port, Faster, Best and Draft; byte-exact PDFs.

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_SETUP := cp "$(TEST_DATA)/systems/system_6_0_8_20mb_8_24gc.img" "$(TEST_TMPDIR)/hd.img"

# AppleTalk starts inactive: the printer port is the ImageWriter's serial
# line, not LocalTalk.
TEST_ARGS := config=appletalk-inactive.json hd=$(TEST_TMPDIR)/hd.img --print-dir=$(TEST_RESULTS_DIR)

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
