TEST_NAME := dump_ditl
TEST_SRCS := test.c
TEST_HARNESS := isolated
EXTRA_SRCS := ../../../../tools/dump/decoders/decoders.c \
              ../../../../tools/dump/decoders/decode_bndl.c \
              ../../../../tools/dump/decoders/decode_dialog.c \
              ../../../../tools/dump/decoders/decode_menu.c \
              ../../../../tools/dump/decoders/decode_size.c \
              ../../../../tools/dump/decoders/decode_str.c \
              ../../../../tools/dump/decoders/decode_vers.c \
              ../../../../src/core/storage/macroman.c
include ../../common.mk

# The per-type resource decoders live in tools/dump/decoders/.
CFLAGS += -I$(WORKSPACE_ROOT)/tools/dump/decoders -I$(WORKSPACE_ROOT)/src/core/storage
