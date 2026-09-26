# The one host keyboard path: DOM codes -> ADB raw codes, the held-key set,
# and every key a Lisa has surviving the trip.  See test.c.

TEST_NAME := host_keys

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
                 -I$(EMU_ROOT)/core/debug \
                 -I$(EMU_ROOT)/core/shell \
                 -I$(EMU_ROOT)/machines/lisa \
                 -DUNIT_TEST_PLATFORM_OVERRIDE \
                 -include $(UNIT_ROOT)/support/platform.h \
                 -include $(UNIT_ROOT)/support/log.h

CFLAGS := $(BASE_CFLAGS) $(INCLUDE_FLAGS)
LDFLAGS ?=

SRCS := $(CURDIR)/test.c \
        $(EMU_ROOT)/core/host_keys.c \
        $(EMU_ROOT)/machines/lisa/lisa_keymap.c \
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
