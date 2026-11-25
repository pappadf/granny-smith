# display.h: the direct-colour DAC table (display_t.dac_lut) in the shared
# pixel decode that screen.save / screen.match go through, and the shared
# hardware-cursor overlay.  See test.c.

TEST_NAME    := display_dac
TEST_SRCS    := test.c
# display.c for the shared cursor overlay; the test supplies main().
TEST_HARNESS := none
STUBS        := assert checkpoint
EXTRA_SRCS   := ../../../../src/core/peripherals/nubus/display.c
include ../../common.mk
