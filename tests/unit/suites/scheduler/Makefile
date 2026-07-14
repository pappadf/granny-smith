# Scheduler pacing unit test (proposal-scheduler-two-modes.md §7).
# Links the real scheduler.c against a fake host clock and a stub CPU sprint,
# then drives scheduler_main_loop with synthetic tick sequences: accumulator
# convergence at 60/59.94/120 Hz, the PACED_MAX_CATCHUP no-spiral cap, the
# backgrounded-tab reset, the turbo first-tick NaN guard, estimator reset on
# mode switch, and the one-guest-timeline (mode-independent CPI) property.

TEST_NAME := scheduler

UNIT_ROOT      := $(abspath ../../)
WORKSPACE_ROOT := $(abspath $(UNIT_ROOT)/../..)
EMU_ROOT       := $(WORKSPACE_ROOT)/src

BUILD_DIR := $(UNIT_ROOT)/build
OBJ_DIR   := $(BUILD_DIR)/obj/$(TEST_NAME)
TARGET    := $(BUILD_DIR)/$(TEST_NAME)

CC ?= gcc
BASE_CFLAGS := -O0 -g -Wall -Wextra
# $(CURDIR) comes FIRST so this suite's platform.h (extern, controllable
# host_time) shadows the support default (static inline, always 0.0).
INCLUDE_FLAGS := -I$(CURDIR) \
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
                 -include $(CURDIR)/platform.h \
                 -include $(UNIT_ROOT)/support/log.h

CFLAGS := $(BASE_CFLAGS) $(INCLUDE_FLAGS)
LDFLAGS ?= -lm

SRCS := $(CURDIR)/test.c \
        $(EMU_ROOT)/core/scheduler/scheduler.c \
        $(UNIT_ROOT)/support/stub_assert.c

ALL_SRCS := $(abspath $(SRCS))

OBJ := $(foreach s,$(ALL_SRCS),$(OBJ_DIR)/$(patsubst $(WORKSPACE_ROOT)/%,%,$(patsubst %.c,%.o,$(s))))
DEP := $(OBJ:.o=.d)

.PHONY: all run clean

all: $(TARGET)

$(OBJ_DIR)/%.o: $(WORKSPACE_ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

run: $(TARGET)
	@$(TARGET)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

-include $(DEP)
