# Integration test: the daemon runs each statement as soon as it is complete
# (S6, F-33, N-41..N-43).
#
# The daemon used to guess where a request ended before running anything --
# read until a newline plus 1 ms of silence, run the lot, close -- and every
# defect was that guess: a 2 KB cap, a fragment dispatched half-read, a
# second send lost, a block silently dropped.  A half-closing client (nc -N,
# Python's shutdown(SHUT_WR)) read as a disconnect and cancelled its own
# scheduler.run.  These are the socket probes that found them, as a test.

TEST_NAME := Daemon statement streaming
TEST_DESC := pipelined, chunked and half-closing daemon clients; incomplete blocks reported; a long stdin line is one statement

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
