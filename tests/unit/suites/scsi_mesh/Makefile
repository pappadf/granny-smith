# The MESH controller.
#
# MESH lived under machines/tnt/ with no handle of its own, taking config_t*
# everywhere and reaching its state through tnt_st(cfg)->mesh.  It could not be
# constructed outside a whole Power Macintosh, so it had no unit tests at all --
# the only controller in the subsystem without them, and the only one whose bugs
# had to be found through a full boot.
#
# This suite is the reason the move was worth doing.
TEST_NAME := scsi_mesh

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
                 -I$(EMU_ROOT)/core/peripherals/pci \
                 -I$(EMU_ROOT)/core/peripherals/pci/cards \
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

# The test supplies the mock bus, the mock guest memory and a recording
# checkpoint stream itself, so no stub modules are linked in.
#
# scsi_msgsession.c is the real IDENTIFY/SDTR/WDTR parser, shared with the
# 53C8xx SCRIPTS engine: pure logic, no emulator dependencies, so the suite
# links the actual code rather than a stub.
#
# (The paragraph that used to sit here described sym53c825.c's register file,
# copied from the scripts53c8xx suite's Makefile.  That file is not in this
# suite and never was.)
SRCS := $(CURDIR)/test.c \
        $(EMU_ROOT)/core/peripherals/scsi.c \
        $(EMU_ROOT)/core/peripherals/scsi_bus.c \
        $(EMU_ROOT)/core/peripherals/scsi_cdrom.c \
        $(EMU_ROOT)/core/peripherals/scsi_mesh.c \
        $(EMU_ROOT)/core/peripherals/scsi_msgsession.c \
        $(EMU_ROOT)/core/object/object.c \
        $(EMU_ROOT)/core/object/parse.c \
        $(EMU_ROOT)/core/object/value.c \
        $(EMU_ROOT)/core/object/meta.c \
        $(EMU_ROOT)/core/object/api.c \
        $(EMU_ROOT)/core/object/value_format.c \
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
