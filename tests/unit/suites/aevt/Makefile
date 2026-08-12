TEST_NAME := aevt
TEST_SRCS := test.c
TEST_HARNESS := isolated
# The codec only: bytes and values in, values and bytes out.  No transport,
# no session layer, no emulator (docs/core/network/ppc_appleevents.md §5–§6).
EXTRA_SRCS := ../../../../src/core/network/appletalk_aevt_codec.c \
              ../../../../src/core/object/value.c
include ../../common.mk
