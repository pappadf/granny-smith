# Integration suite: Macintosh Plus (proposal-integration-test-rework §7)
#
# Grown out of boot-matrix, which already had the target shape (one
# daemon, machine.boot between rows, per-row throughput floors); the rows
# now come from the shared library in ../lib/mac.script and the suite
# absorbs plus-boot-beep, plus-floppy-blank, and scsi.
#
# Every floppy row picks a different one of the four RAM totals Apple
# shipped on a real Plus (four 30-pin SIMM slots, two banks):
#   1.0 MB = 4x 256 KB (factory default) · 2.0 MB = 2x 1 MB + 2 empty
#   2.5 MB = 2x 1 MB + 2x 256 KB (asymmetric) · 4.0 MB = 4x 1 MB (max)
# Sub-4 MB totals depend on the Plus RAM mirror in src/machines/compact/
# plus.c: the ROM hardcodes its exception save area at $3FFC80, which on
# real hardware wraps back into installed RAM through the un-decoded
# address-bus gap.  Without the mirror the first exception hangs boot at
# the "Welcome to Macintosh" splash — so these rows are that mirror's
# regression test.
#
# The two IIcx rows boot-matrix used to carry are gone, not moved: §7
# drops the IIcx 6.0.8 floppy row as transport-redundant (suite-se30's
# row covers direct-SWIM 800K, and IIcx x 6.0.8 stays lit by the GC
# accel pair on the 6.0.8 HD image) and retires the "SSW 7.0" row as a
# media fiction — that 800K set's boot disk actually runs a 6.0.7-class
# System, so the cell it claimed never existed.  Its 2 bpp JMFB coverage
# lives in iicx-video-modes' 16-cell sweep.
#
#   make test-suite-plus
#   make test-suite-plus TEST_VARS="ROW=plus-beep"   one row only
#   make test-suite-plus TEST_VARS="REGEN=1"         recapture goldens

TEST_NAME := Macintosh Plus suite
TEST_DESC := Boot beep, System 2.0/3.2/4.x/6.0.8 floppy rows across all four RAM configs, blank-disk flow, 7.1 HD boot

# Macintosh Plus ROM (shared by every row)
TEST_ROM := roms/plus-v3-4d1f8172.rom

# Start the daemon on a Plus at 1 MB so the first machine.boot in
# test.script has a sensible baseline to re-instantiate from.
TEST_ARGS := model=plus ram=1024

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
