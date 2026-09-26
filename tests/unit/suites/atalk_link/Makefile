# The real LLAP/DDP/NBP/ATP/ASP (appletalk.c) on a fake wire and clock.
#
# No harness: the isolated harness links its own scheduler and checkpoint
# stubs, and this suite needs a scheduler it can drive and interrogate
# (link_harness.c).  Everything above the transport is stubbed in
# stub_upper.c.

TEST_NAME    := atalk_link
TEST_SRCS    := test.c link_harness.c stub_upper.c
TEST_HARNESS := none
STUBS        := assert
EXTRA_SRCS   := ../../../../src/core/network/appletalk.c \
                ../../../../src/core/network/appletalk_asp.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c
include ../../common.mk
