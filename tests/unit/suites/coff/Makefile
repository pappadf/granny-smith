TEST_NAME := coff
TEST_SRCS := test.c
TEST_HARNESS := isolated
EXTRA_SRCS := ../../../../tools/dump/coff.c
include ../../common.mk

# coff.h moved out of src/core/storage/ to tools/dump/ when the dump
# orchestrator became a standalone tool; the test still links the same
# parser, so add the new include path.
CFLAGS += -I$(WORKSPACE_ROOT)/tools/dump
