# The checkpoint stream reader, tested as an untrusted parser.  See test.c.
#
# Deliberately NOT common.mk: the isolated harness links support/stub_checkpoint.c,
# which defines system_read_checkpoint_data_loc, checkpoint_has_error and friends
# -- the very functions under test.  This suite links the real core/checkpoint.c
# and supplies only the handful of symbols it reaches OUTSIDE its own module.

TEST_NAME := checkpoint

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
                 -I$(EMU_ROOT)/core/object \
                 -I$(EMU_ROOT)/core/vfs \
                 -I$(EMU_ROOT)/machines \
                 -DUNIT_TEST_PLATFORM_OVERRIDE \
                 -include $(UNIT_ROOT)/support/platform.h \
                 -include $(UNIT_ROOT)/support/log.h

CFLAGS := $(BASE_CFLAGS) $(INCLUDE_FLAGS)
LDFLAGS ?=

# The object layer is real so checkpoint.c's class descriptor is validated the
# way it is in the emulator; everything past the module boundary (the cmd_*
# handlers, the gs_* platform hooks, get_build_id) is stubbed in test.c, which
# also lets the test drive the build-ID gate.
SRCS := $(CURDIR)/test.c \
        $(EMU_ROOT)/core/checkpoint.c \
        $(EMU_ROOT)/core/object/object.c \
        $(EMU_ROOT)/core/object/parse.c \
        $(EMU_ROOT)/core/object/value.c \
        $(EMU_ROOT)/core/object/meta.c \
        $(UNIT_ROOT)/support/stub_machine_object.c \
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
