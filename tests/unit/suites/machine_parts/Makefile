# The checkpoint parts list (core/machine_parts.c) against the real stream
# reader: a part's name is checked before its block is read.  See test.c.
#
# No harness, for the reason the checkpoint suite gives: the isolated harness
# links support/stub_checkpoint.c, which stubs the reader this suite needs.

TEST_NAME    := machine_parts
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := machine_object assert
EXTRA_SRCS   := ../../../../src/core/job/seam.c \
                ../../../../src/core/checkpoint.c \
                ../../../../src/core/machine_parts.c \
                ../../../../src/core/io/io_worker.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c
include ../../common.mk
