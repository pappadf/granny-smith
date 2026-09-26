# Mac Plus keyboard: the INQUIRY timeout an INSTANT supersedes.  See test.c.

TEST_NAME    := keyboard
TEST_SRCS    := test.c
# The test supplies its own checkpoint stubs; the only support stub linked in
# is stub_assert.
TEST_HARNESS := none
STUBS        := assert
EXTRA_SRCS   := ../../../../src/core/peripherals/keyboard.c
include ../../common.mk
