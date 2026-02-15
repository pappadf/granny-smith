## Shared build logic for unit test sub-makefiles
#
# Usage inside a test directory Makefile (under suites/<name>/):
#   TEST_NAME    := mytest           # (required) test binary name
#   TEST_SRCS    := test.c           # (required) test sources (relative to test dir)
#   TEST_HARNESS := isolated         # (required) isolated or cpu
#   EXTRA_SRCS   := ../../../src/... # (optional) additional source files
#   EXTRA_CFLAGS := -DFOO            # (optional) additional compiler flags
#   include ../../common.mk
#
# Harness modes:
#   isolated  Pure unit tests, no emulator subsystems (only stubs)
#   cpu       CPU tests with real memory and CPU
#
# Dependency tracking: automatic via -MMD -MP.
# Header changes trigger correct recompilations.

ifndef TEST_NAME
$(error TEST_NAME not set before including common.mk)
endif

# Default to isolated mode
TEST_HARNESS ?= isolated

# -- Root directories --

UNIT_ROOT      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
WORKSPACE_ROOT := $(abspath $(UNIT_ROOT)/../..)
EMU_ROOT       := $(WORKSPACE_ROOT)/src

# -- Build directories --
# Objects go under BUILD_DIR/obj/<test>/ to avoid collision with binary.

BUILD_DIR  := $(UNIT_ROOT)/build
OBJ_DIR    := $(BUILD_DIR)/obj/$(TEST_NAME)
TARGET     := $(BUILD_DIR)/$(TEST_NAME)

# -- Compiler and flags --

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

CFLAGS  := $(BASE_CFLAGS) $(INCLUDE_FLAGS)
LDFLAGS ?=
LDFLAGS += -rdynamic

# -- Harness and stub configuration --

EMU_SRCS ?=

ifeq ($(TEST_HARNESS),isolated)
  # Isolated mode: stub-only harness, no real emulator subsystems
  HARNESS_SRCS := $(UNIT_ROOT)/support/harness_common.c \
                  $(UNIT_ROOT)/support/harness_isolated.c
  STUB_SRCS := $(UNIT_ROOT)/support/stub_platform.c \
               $(UNIT_ROOT)/support/stub_shell.c \
               $(UNIT_ROOT)/support/stub_checkpoint.c \
               $(UNIT_ROOT)/support/stub_system.c \
               $(UNIT_ROOT)/support/stub_memory.c \
               $(UNIT_ROOT)/support/stub_debugger.c \
               $(UNIT_ROOT)/support/stub_peripherals.c \
               $(UNIT_ROOT)/support/stub_assert.c
  COMMON_SRCS := $(HARNESS_SRCS) $(STUB_SRCS)

else ifeq ($(TEST_HARNESS),cpu)
  # CPU mode: harness with real CPU and memory
  HARNESS_SRCS := $(UNIT_ROOT)/support/harness_common.c \
                  $(UNIT_ROOT)/support/harness_cpu.c
  STUB_SRCS := $(UNIT_ROOT)/support/stub_platform.c \
               $(UNIT_ROOT)/support/stub_shell.c \
               $(UNIT_ROOT)/support/stub_checkpoint.c \
               $(UNIT_ROOT)/support/stub_system.c \
               $(UNIT_ROOT)/support/stub_debugger.c \
               $(UNIT_ROOT)/support/stub_peripherals.c \
               $(UNIT_ROOT)/support/stub_assert.c
  EMU_SRCS += $(EMU_ROOT)/core/cpu/cpu.c \
              $(EMU_ROOT)/core/cpu/cpu_68000.c \
              $(EMU_ROOT)/core/cpu/cpu_disasm.c \
              $(EMU_ROOT)/core/memory/memory.c
  COMMON_SRCS := $(HARNESS_SRCS) $(STUB_SRCS)

else
  $(error Invalid TEST_HARNESS '$(TEST_HARNESS)'. Use: isolated or cpu)
endif

# -- Source and object file collection --
# All source paths are converted to absolute, then mapped to object
# files under OBJ_DIR mirroring the workspace-relative directory
# structure.  This avoids name collisions across directories.

ALL_SRCS := $(abspath $(addprefix $(CURDIR)/,$(TEST_SRCS))) \
            $(abspath $(COMMON_SRCS)) \
            $(abspath $(EMU_SRCS)) \
            $(abspath $(EXTRA_SRCS))

# /workspaces/granny-smith/some/path/foo.c -> $(OBJ_DIR)/some/path/foo.o
OBJ := $(foreach s,$(ALL_SRCS),$(OBJ_DIR)/$(patsubst $(WORKSPACE_ROOT)/%,%,$(patsubst %.c,%.o,$(s))))
DEP := $(OBJ:.o=.d)

# -- Build rules --

.PHONY: all run clean

all: $(TARGET)

# Compile: workspace source -> object under OBJ_DIR
$(OBJ_DIR)/%.o: $(WORKSPACE_ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -MMD -MP -c $< -o $@

# Link: objects -> test binary
$(TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	@echo "[LD ] $@"
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

# Include auto-generated header dependency files
-include $(DEP)

# Run the test binary
run: $(TARGET)
	$(TARGET)

# Clean this test's artifacts
clean:
	rm -f $(TARGET)
	rm -rf $(OBJ_DIR)
