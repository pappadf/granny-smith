# The record-ring primitive (src/core/mailbox/mailbox_ring.c) and the index
# arithmetic under it (spsc_index.h): one writer and one reader on a small
# ring in one process.  See test.c.

TEST_NAME    := mailbox_ring
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := assert
EXTRA_SRCS   := ../../../../src/core/mailbox/mailbox_ring.c
include ../../common.mk
