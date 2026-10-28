TEST_NAME := dump_pef
TEST_SRCS := test.c
TEST_HARNESS := isolated
EXTRA_SRCS := ../../../../tools/dump/pef.c
include ../../common.mk

# The PEF parser lives in tools/dump/.
CFLAGS += -I$(WORKSPACE_ROOT)/tools/dump
