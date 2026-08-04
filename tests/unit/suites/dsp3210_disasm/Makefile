# DSP3210 disassembler unit test (proposal-dsp3210-plaintalk.md Phase 1).
# The 100 encoding vectors of the validated reference disassembler
# (IM worked examples, ROM-verified dossier words, the illegal-opcode
# list) against src/core/cpu/dsp3210/dsp3210_disasm.c.  Dependency-free
# C99 — no stubs.

TEST_NAME := dsp3210_disasm

UNIT_ROOT      := $(abspath ../../)
WORKSPACE_ROOT := $(abspath $(UNIT_ROOT)/../..)
EMU_ROOT       := $(WORKSPACE_ROOT)/src

BUILD_DIR := $(UNIT_ROOT)/build
OBJ_DIR   := $(BUILD_DIR)/obj/$(TEST_NAME)
TARGET    := $(BUILD_DIR)/$(TEST_NAME)

CC ?= gcc
BASE_CFLAGS := -O0 -g -Wall -Wextra
INCLUDE_FLAGS := -I$(EMU_ROOT)/core/cpu/dsp3210

CFLAGS := $(BASE_CFLAGS) $(INCLUDE_FLAGS)
LDFLAGS ?=

SRCS := $(CURDIR)/test.c \
        $(EMU_ROOT)/core/cpu/dsp3210/dsp3210_disasm.c

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
