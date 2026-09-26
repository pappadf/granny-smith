# Scheduler pacing unit test.
# Links the real scheduler.c against a fake host clock and a stub CPU sprint,
# then drives scheduler_main_loop with synthetic tick sequences: accumulator
# convergence at 60/59.94/120 Hz, the PACED_MAX_CATCHUP no-spiral cap, the
# backgrounded-tab reset, the turbo first-tick NaN guard, estimator reset on
# mode switch, and the one-guest-timeline (mode-independent CPI) property.

TEST_NAME    := scheduler
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := assert
EXTRA_SRCS   := ../../../../src/core/scheduler/scheduler.c
# $(CURDIR) comes FIRST so this suite's platform_clock.h (extern, controllable
# host_time) shadows the support default (static inline, always 0.0).  That
# order is why the suite names its own list rather than taking the default;
# it is expanded after common.mk has set UNIT_ROOT and EMU_ROOT, hence `=`.
INCLUDE_FLAGS = -I$(CURDIR) \
                -I$(UNIT_ROOT)/support \
                -I$(EMU_ROOT)/core \
                -I$(EMU_ROOT)/core/cpu \
                -I$(EMU_ROOT)/core/memory \
                -I$(EMU_ROOT)/core/peripherals \
                -I$(EMU_ROOT)/core/scheduler \
                -I$(EMU_ROOT)/core/debug \
                -I$(EMU_ROOT)/core/storage \
                -I$(EMU_ROOT)/core/shell \
                -I$(EMU_ROOT)/core/object \
                -I$(EMU_ROOT)/core/vfs \
                -DUNIT_TEST_PLATFORM_OVERRIDE \
                -include $(UNIT_ROOT)/support/platform.h \
                -include $(UNIT_ROOT)/support/log.h
include ../../common.mk
