# display_ramdac.h: the shared RAMDAC palette-port sequencer (ramdac_seq_t).
# Run-write, run-read, peek, re-addressing mid-entry, the split VGA indices
# and the four-component (Sebastian) mode.  See test.c.

TEST_NAME    := display_ramdac
TEST_SRCS    := test.c
# display_ramdac.h is header-only; the test supplies main().
TEST_HARNESS := none
STUBS        := assert
include ../../common.mk
