# Memory unit test
# Tests parameterised memory_map_init() with 24-bit and 32-bit address spaces.
# Uses real memory.c instead of stub_memory.c to test the actual implementation.

TEST_NAME := memory

UNIT_ROOT      := $(abspath ../../)
WORKSPACE_ROOT := $(abspath $(UNIT_ROOT)/../..)
EMU_ROOT       := $(WORKSPACE_ROOT)/src

BUILD_DIR := $(UNIT_ROOT)/build
OBJ_DIR   := $(BUILD_DIR)/obj/$(TEST_NAME)
TARGET    := $(BUILD_DIR)/$(TEST_NAME)

CC ?= gcc
BASE_CFLAGS := -O0 -g -Wall -Wextra
INCLUDE_FLAGS := -I$(UNIT_ROOT)/support \
                 -I$(EMU_ROOT)/core \
                 -I$(EMU_ROOT)/core/cpu \
                 -I$(EMU_ROOT)/core/memory \
                 -I$(EMU_ROOT)/core/peripherals \
                 -I$(EMU_ROOT)/core/scheduler \
                 -I$(EMU_ROOT)/core/debug \
                 -I$(EMU_ROOT)/core/storage \
                 -I$(EMU_ROOT)/core/network \
                 -I$(EMU_ROOT)/core/shell \
                 -I$(EMU_ROOT)/platform/wasm \
                 -DUNIT_TEST_PLATFORM_OVERRIDE \
                 -include $(UNIT_ROOT)/support/platform.h \
                 -include $(UNIT_ROOT)/support/log.h

CFLAGS := $(BASE_CFLAGS) $(INCLUDE_FLAGS)
LDFLAGS ?=
LDFLAGS += -rdynamic

# Real memory.c is included; stub_memory.c is excluded to avoid symbol conflicts
SRCS := $(CURDIR)/test.c \
        $(UNIT_ROOT)/support/harness_common.c \
        $(UNIT_ROOT)/support/harness_isolated.c \
        $(UNIT_ROOT)/support/stub_platform.c \
        $(UNIT_ROOT)/support/stub_shell.c \
        $(UNIT_ROOT)/support/stub_checkpoint.c \
        $(UNIT_ROOT)/support/stub_system.c \
        $(UNIT_ROOT)/support/stub_debugger.c \
        $(UNIT_ROOT)/support/stub_peripherals.c \
        $(UNIT_ROOT)/support/stub_assert.c \
        $(EMU_ROOT)/core/memory/memory.c

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
	@echo "[LD ] $@"
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

-include $(DEP)

run: $(TARGET)
	$(TARGET)

clean:
	rm -f $(TARGET)
	rm -rf $(OBJ_DIR)
