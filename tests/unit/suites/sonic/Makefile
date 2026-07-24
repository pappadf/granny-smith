# DP83932 SONIC chip-model unit test (Quadra proposal Phase F).
# Links sonic.c against mock guest memory (sonic_set_memory_hooks) so the
# scsi flows are replaced by SONIC descriptor DMA over the mock buffer.
# Apple ROM self-tests pin are exercised deterministically.


TEST_NAME := sonic

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
                 -I$(EMU_ROOT)/core/shell \
                 -I$(EMU_ROOT)/core/object \
                 -I$(EMU_ROOT)/core/vfs \
                 -DUNIT_TEST_PLATFORM_OVERRIDE \
                 -include $(UNIT_ROOT)/support/platform.h \
                 -include $(UNIT_ROOT)/support/log.h

CFLAGS := $(BASE_CFLAGS) $(INCLUDE_FLAGS)
LDFLAGS ?=

SRCS := $(CURDIR)/test.c \
        $(EMU_ROOT)/core/peripherals/sonic.c \
        $(UNIT_ROOT)/support/stub_checkpoint.c \
        $(UNIT_ROOT)/support/stub_system.c \
        $(UNIT_ROOT)/support/harness_common.c \
        $(UNIT_ROOT)/support/harness_isolated.c \
        $(UNIT_ROOT)/support/stub_platform.c

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
