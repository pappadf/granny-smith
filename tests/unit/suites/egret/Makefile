# Egret: the send-abandon watchdog that keeps the transport from wedging.  See test.c.

TEST_NAME    := egret
TEST_SRCS    := test.c
# The test supplies its own scheduler, VIA, RTC, ADB and checkpoint-stream
# mocks, so assert is the only support stub linked in.
TEST_HARNESS := none
STUBS        := assert
EXTRA_SRCS   := ../../../../src/machines/mdu/egret.c
include ../../common.mk
