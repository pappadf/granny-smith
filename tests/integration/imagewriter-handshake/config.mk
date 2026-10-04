# Integration test: the ImageWriter's simulated 2 KB buffer and its busy line
#
# A Macintosh Plus with no disk sits at its boot prompt; the test script
# plays the host itself.  It sets the printer port (SCC channel B) up for
# asynchronous 8N1 output and writes 4000 bytes of text to it, one byte at a
# time, each only when the transmitter is empty and CTS says the printer is
# ready -- what a driver using the hardware handshake does.  The printer
# (`buffer_model = "2k"`) prints far more slowly than the host sends, so its
# buffer fills: it drops its DTR (the Mac's CTS) with 30 bytes left, raises
# it again at 100, and the host waits each time.  At the end nothing was
# dropped, every byte was printed, and the PDF is the golden.
#
# What it guards: the buffer model's fill and drain, the busy thresholds,
# the ready line reaching RR0 through the SCC port-device seam, and the
# SCC's per-character transmit pacing towards a device.

TEST_NAME := ImageWriter buffer and handshake
TEST_DESC := A scripted host fills the simulated 2 KB buffer through the SCC, waits on CTS each time the printer goes busy; nothing is lost.

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_ARGS := --print-dir=$(TEST_RESULTS_DIR)

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
