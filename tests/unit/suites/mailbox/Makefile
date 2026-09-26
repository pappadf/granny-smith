# The mailbox (src/core/mailbox/mailbox.c): a client in the same process
# writes REQ_EVAL records and reads EVT_RESULT records while the drain
# serves them through a stub evaluator.  Small rings force wraps and a
# held-back result.  See test.c.

TEST_NAME    := mailbox
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := assert
EXTRA_SRCS   := ../../../../src/core/mailbox/mailbox.c \
                ../../../../src/core/mailbox/mailbox_ring.c
EXTRA_CFLAGS := -DGS_MBX_REQ_BYTES=4096u -DGS_MBX_EVT_BYTES=4096u
include ../../common.mk
