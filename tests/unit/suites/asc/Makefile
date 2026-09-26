# Apple Sound Chip unit test.
# Links the real asc.c against recording stubs for the scheduler, VIA and
# audio_out sink; replays the ROM POST register vectors (USTNonCritTsts.a
# `sndrnold`/`sndrdold` — the standard-ASC behavioral spec) and pins the FIFO
# IRQ state machine and the sample-rate producer directly, with no emulator,
# ROM or MMU involved.

TEST_NAME    := asc
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := checkpoint
EXTRA_SRCS   := ../../../../src/core/peripherals/asc.c
include ../../common.mk
