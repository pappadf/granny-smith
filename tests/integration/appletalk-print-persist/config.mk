# Integration test: the emulated LaserWriter keeps what a job downloads for
# as long as the printer lives -- and a change of machine restarts it
#
# The printer is an interpreter on the AppleTalk network, not part of the
# Mac (src/core/network/laserwriter_job.h, "The printer"): each job is
# reverted at its end except what it made permanent with exitserver, which
# every later job inherits -- as a LaserWriter keeps a downloaded procset
# until it is switched off.  machine.restart keeps it; appletalk.printer.restart()
# is its own power switch; and a change of machine -- a checkpoint load or a
# machine.boot -- restarts it, so no job runs on from one machine into the
# next.
#
# The classic LaserWriter driver shows it without any instrumentation: it
# asks the printer whether its PatchPrep procset is resident and, when not,
# uploads it inside `serverdict begin exitserver`.  So the first print on a
# printer makes one job permanent, and a later print on the same printer
# skips the upload and needs one PAP job fewer.  The row prints five times
# on the appletalk-print machine (Plus, System 6.0.8, LaserWriter 7.0) and
# reads appletalk.printer.interpreter_jobs / interpreter_permanent_jobs:
#
#   1. first print                      uploads PatchPrep (permanent: 1)
#   2. second print                     no upload, fewer jobs
#   3. machine.restart, print           the printer was kept: no upload
#   4. appletalk.printer.restart(), print   a new printer: uploads again
#   -  checkpoint.save, checkpoint.load     a new printer: counters back at 0
#   5. machine.boot, Chooser, print     a new machine, a new printer: uploads again
#
# GATED ON THE INTERPRETER, like the other print rows: a PLATEN=0 binary has
# no bridge, so the row logs a skip and passes.  To run it for real:
#
#   make -C tests/integration test-appletalk-print-persist PLATEN=1

TEST_NAME := LaserWriter printer lifetime (platen)
TEST_DESC := Five prints across a machine.restart, a printer restart, a checkpoint load and a machine.boot: exitserver downloads live as long as the printer, which a change of machine restarts.

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_SETUP := cp "$(TEST_DATA)/systems/system_6_0_8_20mb_8_24gc.img" "$(TEST_TMPDIR)/hd.img"

# HD names the disk again for the machine.boot leg (a new machine inherits
# no media).
# AppleTalk starts inactive, so the Chooser's "LaserWriter requires
# AppleTalk" step this test walks through appears.
TEST_ARGS := config=appletalk-inactive.json hd=$(TEST_TMPDIR)/hd.img --print-dir=$(TEST_RESULTS_DIR) --var HD=$(TEST_TMPDIR)/hd.img

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
