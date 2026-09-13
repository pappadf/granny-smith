# SCSI checkpoint round-trip.
#
# 03-scsi F-20: the save was a plain-data prefix that stopped at `devices` plus
# a hand-written per-field loop, and the loop forgot fields.  sense,
# prevent_removal and default_block_size were all silently dropped, so a
# checkpoint taken between a CHECK CONDITION and the driver's REQUEST SENSE
# restored as NO SENSE -- measured happening twice in suite-quadra, once with
# UNIT ATTENTION / 0x29 pending.
#
# The fix moves the image pointers out of the device records so the whole
# plain-data region saves in one write, the way via_t, scc_t and rtc_t do.
# This suite exists so the NEXT field added to that region is caught if it
# does not survive: it stages state across the whole struct, round-trips it,
# and compares.

TEST_NAME := scsi_checkpoint

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
# sym53c825.c comes along for the register file.  The engine addresses
# registers by 7-bit number and now goes through the same accessors the host
# BAR windows use, so the read-to-clear and side-effect semantics are exercised
# rather than stubbed -- which is the point of F-14.  Its PCI surface stays out
# of the way behind the pci_* stubs the test already supplies.
SRCS := $(CURDIR)/test.c \
        $(EMU_ROOT)/core/peripherals/scsi.c \
        $(EMU_ROOT)/core/peripherals/scsi_bus.c \
        $(EMU_ROOT)/core/peripherals/scsi_cdrom.c \
        $(EMU_ROOT)/core/object/object.c \
        $(EMU_ROOT)/core/object/value.c \
        $(EMU_ROOT)/core/object/meta.c \
        $(EMU_ROOT)/core/object/api.c

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
