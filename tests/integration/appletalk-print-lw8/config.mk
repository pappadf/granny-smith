# Integration test: LaserWriter 8 print over AppleTalk, and how long it takes
#
# The third print row, beside appletalk-print (System 6.0.8, LaserWriter 7.0)
# and appletalk-print-71 (System 7.1, LaserWriter 7.1.2).  This one prints the
# same way -- Chooser, then the Finder's "Print Window..." -- from System 7.5
# on a IIci with LaserWriter 8.1.1, the driver that asks the printer about
# itself before every job.
#
# It exists because of a performance defect.  LaserWriter 8 opens each job
# with a query (language level, resolution, binary channel, fonts) and reads
# the answers from the PAP read channel in order.  The printer used to answer
# each of the driver's read credits at once with a status line, so the
# driver took "status: busy" for its first answer, assumed a Level 1 printer
# without resident fonts, and sent Level 1 procsets plus Helvetica converted
# from TrueType: 346 KB in two jobs, where the real answers get one 17.5 KB
# job.  In this row that was 32.6 s of guest time from Print to PDF against
# 15.4 s (with LaserWriter 8.3.4 on System 7.5.3, 52 s against 14 s).  The
# read channel now carries only the program's own output.
#
# The assertions are the defect's three symptoms, in guest time, which is
# deterministic: the whole print (Print button to PDF) must take less than
# 30 s, the driver must send less than 64 KB, and it must send one job (the
# query and the document together, as it does once it reads the answers).
#
# GATED ON THE INTERPRETER, like the other print rows: a PLATEN=0 binary has
# no bridge, so the row logs a skip and passes.  To run it for real:
#
#   make -C tests/integration test-appletalk-print-lw8 PLATEN=1
#
# The PDF lands in the results directory (--print-dir).

TEST_NAME := LaserWriter 8 print speed (platen)
TEST_DESC := IIci + System 7.5 + LaserWriter 8.1.1 -> Chooser -> Print Window -> a PDF in under 30 s of guest time, one job under 64 KB.

TEST_ROM := roms/iici-368cadfe.rom

# The script re-boots with the built-in video and attaches the base image
# directly, as appletalk-print-71 does: printing writes the spool file to the
# image's delta beside it, never into the base.
TEST_ARGS := model=iici ram=16384 --print-dir=$(TEST_RESULTS_DIR)

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
