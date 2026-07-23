# Lisa/XL ROM interleave + identification unit test (Step 1 of the Lisa-XL work)
# Links the real rom.c against real memory.c/mmu.c/object/value/meta,
# with stub system/cpu, to exercise interleaving and signature identification.

TEST_NAME := rom

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
                 -I$(EMU_ROOT)/core/peripherals/nubus \
                 -I$(EMU_ROOT)/core/scheduler \
                 -I$(EMU_ROOT)/core/debug \
                 -I$(EMU_ROOT)/core/storage \
                 -I$(EMU_ROOT)/core/network \
                 -I$(EMU_ROOT)/core/shell \
                 -I$(EMU_ROOT)/core/object \
                 -I$(EMU_ROOT)/core/vfs \
                 -I$(EMU_ROOT)/machines \
                 -I$(EMU_ROOT)/platform/wasm \
                 -DUNIT_TEST_PLATFORM_OVERRIDE \
                 -include $(UNIT_ROOT)/support/platform.h \
                 -include $(UNIT_ROOT)/support/log.h

CFLAGS := $(BASE_CFLAGS) $(INCLUDE_FLAGS)
LDFLAGS ?=
LDFLAGS += -rdynamic

SRCS := $(CURDIR)/test.c \
        $(CURDIR)/stub_lisa_system.c \
        $(UNIT_ROOT)/support/harness_common.c \
        $(UNIT_ROOT)/support/harness_isolated.c \
        $(UNIT_ROOT)/support/stub_platform.c \
        $(UNIT_ROOT)/support/stub_shell.c \
        $(UNIT_ROOT)/support/stub_checkpoint.c \
        $(UNIT_ROOT)/support/stub_assert.c \
        $(UNIT_ROOT)/support/stub_cpu.c \
        $(EMU_ROOT)/core/memory/rom.c \
        $(EMU_ROOT)/core/memory/memory.c \
        $(EMU_ROOT)/core/memory/mmu.c \
        $(EMU_ROOT)/core/machine_config.c \
        $(EMU_ROOT)/core/object/meta.c \
        $(EMU_ROOT)/core/object/object.c \
        $(EMU_ROOT)/core/object/value.c

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
