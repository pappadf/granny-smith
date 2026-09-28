# Integration test: LaserWriter print over AppleTalk with the platen interpreter
#
# Boots System 6.0.8 on a Plus, publishes the emulated LaserWriter, and drives
# the guest Chooser + Finder "Print Directory..." with injected mouse input —
# the same flow that produced the captured job in gs-test-data/printjobs.  The
# assertions are the object model's own values (appletalk.printer.documents /
# last_pages / last_outcome / status), not pixels, so the row proves the PAP ->
# platen bridge ran end to end and returned to idle.
#
# GATED ON THE INTERPRETER.  The bridge is only compiled in with a PLATEN=1
# build (EfterScript's platen library linked); the integration harness builds
# the default PLATEN=0 headless binary, where appletalk.printer.interpreter is
# false and this row logs a skip and passes.  To run it for real:
#
#   cargo build -p platen --release            # in ../efterscript
#   make -C ../.. -f Makefile.headless PLATEN=1
#   make -C tests/integration test-appletalk-print
#
# The PDF is written to the results directory (--print-dir); a PLATEN=0 run
# writes nothing.

TEST_NAME := LaserWriter Print (platen)
TEST_DESC := Chooser -> LaserWriter -> Finder Print Directory -> a PDF via the platen interpreter; idle afterward.

TEST_ROM := roms/plus-v3-4d1f8172.rom

# A private copy: printing writes nothing to the guest volume, but keep the
# base image pristine like the other System 6 rows.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_6_0_8_20mb_8_24gc.img" "$(TEST_TMPDIR)/hd.img"

TEST_ARGS := hd=$(TEST_TMPDIR)/hd.img --print-dir=$(TEST_RESULTS_DIR)

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
