# RBV built-in video's VDAC (Bt450) unit test.
# Links the real builtin_rbv_video.c against local bus stubs and drives the
# VDAC ports as the machine's register window does: run-write, run-read,
# and the peek that reports a read without taking it.

TEST_NAME    := rbv_vdac
TEST_SRCS    := test.c
TEST_HARNESS := isolated
OMIT_STUBS   := shell memory debugger peripherals
EXTRA_SRCS   := ../../../../src/machines/mdu/builtin_rbv_video.c
include ../../common.mk
