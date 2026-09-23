# The real LLAP/DDP/NBP/ATP/ASP (appletalk.c) on a fake wire and clock.
#
# Standalone rather than common.mk: the isolated harness links its own
# scheduler and checkpoint stubs, and this suite needs a scheduler it can
# drive and interrogate (link_harness.c).  Everything above the transport is
# stubbed in stub_upper.c.

TEST_NAME := atalk_link

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
                 -I$(EMU_ROOT)/core/network \
                 -I$(EMU_ROOT)/core/object \
                 -I$(EMU_ROOT)/core/shell \
                 -I$(EMU_ROOT)/core/storage \
                 -I$(EMU_ROOT)/core/vfs \
                 -I$(EMU_ROOT)/peeler/include \
                 -I$(EMU_ROOT)/machines \
                 -DUNIT_TEST_PLATFORM_OVERRIDE \
                 -include $(UNIT_ROOT)/support/platform.h \
                 -include $(UNIT_ROOT)/support/log.h

CFLAGS := $(BASE_CFLAGS) $(INCLUDE_FLAGS)
LDFLAGS ?=

SRCS := $(CURDIR)/test.c \
        $(CURDIR)/link_harness.c \
        $(CURDIR)/stub_upper.c \
        $(EMU_ROOT)/core/network/appletalk.c \
        $(EMU_ROOT)/core/object/object.c \
        $(EMU_ROOT)/core/object/parse.c \
        $(EMU_ROOT)/core/object/value.c \
        $(EMU_ROOT)/core/object/meta.c \
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
