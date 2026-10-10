# The mailbox (src/core/mailbox/mailbox.c): a client in the same process
# writes REQ_EVAL records and reads EVT_RESULT records while the drain
# serves them through a stub evaluator, or through object_eval for the result
# limit.  Small rings force wraps and a held-back result.  See test.c.

TEST_NAME    := mailbox
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := assert
EXTRA_SRCS   := ../../../../src/core/mailbox/mailbox.c \
                ../../../../src/core/mailbox/mailbox_ring.c \
                ../../../../src/core/job/job.c \
                ../../../../src/core/job/seam.c \
                ../../../../src/core/io/io_worker.c \
                ../../../../src/core/storage/io_leaf.c \
                ../../../../src/core/mailbox/out.c \
                ../../../../src/core/object/api.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/value_format.c
EXTRA_CFLAGS := -DGS_MBX_REQ_BYTES=4096u -DGS_MBX_EVT_BYTES=4096u
include ../../common.mk
