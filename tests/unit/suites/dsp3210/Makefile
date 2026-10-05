# DSP3210 core unit test.
# Links the real src/core/cpu/dsp3210/dsp3210.c — the core is
# dependency-free portable C99, so no stubs are needed: the 31 ported
# reference tests plus the repo-adaptation tests (timer/BIO MMIO, PS.IR
# mirrors, run/idle contract, mock-bus bootstrap handshake) run against
# the core alone.  It compiles against its own directory only, with none of
# the unit support shims.

TEST_NAME     := dsp3210
TEST_SRCS     := test.c
TEST_HARNESS  := none
INCLUDE_FLAGS  = -I$(EMU_ROOT)/core/cpu/dsp3210
EXTRA_SRCS    := ../../../../src/core/cpu/dsp3210/dsp3210.c
include ../../common.mk
