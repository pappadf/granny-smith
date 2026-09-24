TEST_NAME := aevt_inbox
TEST_SRCS := test.c stub_ppc.c
TEST_HARNESS := isolated
# The Apple-event endpoint (appletalk_aevt.c) with its codec and the object
# model it publishes through; the PPC session layer beneath it is a stub that
# records what is written (stub_ppc.c).  Inbound events are handed straight to
# atalk_aevt_deliver, as the session layer would.
EXTRA_SRCS := ../../../../src/core/network/appletalk_aevt.c \
              ../../../../src/core/network/appletalk_aevt_codec.c \
              ../../../../src/core/object/value.c \
              ../../../../src/core/object/object.c \
              ../../../../src/core/object/parse.c \
              ../../../../src/core/object/meta.c \
              ../../../../src/core/object/expr.c \
              ../../../../src/core/object/value_format.c
include ../../common.mk
