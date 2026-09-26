## Shared build logic for unit test sub-makefiles
#
# Usage inside a test directory Makefile (under suites/<name>/):
#   TEST_NAME     := mytest           # (required) test binary name
#   TEST_SRCS     := test.c           # (required) test sources (relative to test dir)
#   TEST_HARNESS  := isolated         # isolated (default), cpu, or none
#   EXTRA_SRCS    := ../../../src/... # (optional) additional source files
#   EXTRA_CFLAGS  := -DFOO            # (optional) additional compiler flags
#   EXTRA_LDFLAGS := -lz              # (optional) additional link flags
#   STUBS         := assert           # (optional) support/stub_<name>.c to add
#   OMIT_STUBS    := memory           # (optional) harness stubs to leave out
#   SANITIZE      := -fsanitize=...   # (optional) sanitizer flags, compile + link
#   INCLUDE_FLAGS := -I...            # (optional) replaces the default list
#   RUN_ENV       := VAR=value        # (optional) environment for the run
#   include ../../common.mk
#
# Harness modes:
#   isolated  Pure unit tests, no emulator subsystems (only stubs)
#   cpu       CPU tests with real memory and CPU
#   none      No harness and no default stubs: the test supplies main() and
#             its own mocks, and names any support stubs it wants in STUBS
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

BUILD_DIR  ?= $(UNIT_ROOT)/build
OBJ_DIR    := $(BUILD_DIR)/obj/$(TEST_NAME)
# TARGET_EXT names the wasm32 run's output (.js) apart from the native one.
TARGET_EXT ?=
TARGET     := $(BUILD_DIR)/$(TEST_NAME)$(TARGET_EXT)

# -- Compiler and flags --

# make's built-in CC=cc outranks `CC ?=`; replace only that default.
ifeq ($(origin CC),default)
CC := gcc
endif

BASE_CFLAGS := -O0 -g -Wall -Wextra

# The core and machine header directories are the ones both product builds
# use (src/sources.mk); the unit build puts its support shims first and
# builds against the wasm platform headers.  A suite that deliberately
# compiles against a narrower set sets INCLUDE_FLAGS itself.
CORE_DIR     := $(EMU_ROOT)/core
MACHINES_DIR := $(EMU_ROOT)/machines
PEELER_DIR   := $(EMU_ROOT)/peeler
include $(WORKSPACE_ROOT)/src/sources.mk

INCLUDE_FLAGS ?= -I$(UNIT_ROOT)/support \
                 $(CORE_INCLUDES) \
                 -I$(EMU_ROOT)/platform/wasm \
                 $(PEELER_INCLUDES) \
                 -DUNIT_TEST_PLATFORM_OVERRIDE \
                 -include $(UNIT_ROOT)/support/platform.h \
                 -include $(UNIT_ROOT)/support/log.h

SANITIZE ?=

CFLAGS  := $(BASE_CFLAGS) $(SANITIZE) $(INCLUDE_FLAGS)
LDFLAGS ?=
LDFLAGS += -rdynamic -lm $(SANITIZE)
EXTRA_LDFLAGS ?=

# -- Harness and stub configuration --

EMU_SRCS ?=
STUBS ?=
OMIT_STUBS ?=

ifeq ($(TEST_HARNESS),none)
  HARNESS_SRCS :=
  STUB_SRCS :=
  COMMON_SRCS :=

else ifeq ($(TEST_HARNESS),isolated)
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
               $(UNIT_ROOT)/support/stub_lisa_mmu.c \
               $(UNIT_ROOT)/support/stub_assert.c
  EMU_SRCS += $(EMU_ROOT)/core/cpu/cpu.c \
              $(EMU_ROOT)/core/cpu/cpu_68000.c \
              $(EMU_ROOT)/core/cpu/cpu_68030.c \
              $(EMU_ROOT)/core/cpu/cpu_68040.c \
              $(EMU_ROOT)/core/cpu/cpu_disasm.c \
              $(EMU_ROOT)/core/cpu/fpu.c \
              $(EMU_ROOT)/core/cpu/fpu_transc.c \
              $(EMU_ROOT)/core/memory/memory.c \
              $(EMU_ROOT)/core/memory/mmu.c \
              $(EMU_ROOT)/core/memory/mmu040.c \
              $(EMU_ROOT)/core/object/alias.c \
              $(EMU_ROOT)/core/object/meta.c \
              $(EMU_ROOT)/core/object/object.c \
              $(EMU_ROOT)/core/object/parse.c \
              $(EMU_ROOT)/core/object/value.c
  COMMON_SRCS := $(HARNESS_SRCS) $(STUB_SRCS)

else
  $(error Invalid TEST_HARNESS '$(TEST_HARNESS)'. Use: isolated, cpu or none)
endif

# Harness stubs a suite replaces with the real module (e.g. memory), and
# support stubs it adds by name.
COMMON_SRCS := $(filter-out $(foreach s,$(OMIT_STUBS),$(UNIT_ROOT)/support/stub_$(s).c),$(COMMON_SRCS)) \
               $(foreach s,$(STUBS),$(UNIT_ROOT)/support/stub_$(s).c)

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

# Objects depend on the flags they were compiled with: a stamp named after a
# hash of the compile flags, so changing EXTRA_CFLAGS, SANITIZE or CC
# rebuilds the suite instead of linking objects compiled the old way.
FLAGS_HASH  := $(shell printf '%s' '$(subst ','\'',$(CC) $(CFLAGS) $(EXTRA_CFLAGS))' | md5sum | cut -c1-12)
FLAGS_STAMP := $(OBJ_DIR)/flags-$(FLAGS_HASH).stamp
$(FLAGS_STAMP):
	@mkdir -p $(dir $@)
	@rm -f $(OBJ_DIR)/flags-*.stamp
	@touch $@
$(OBJ): $(FLAGS_STAMP)

# Compile: workspace source -> object under OBJ_DIR
$(OBJ_DIR)/%.o: $(WORKSPACE_ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -MMD -MP -c $< -o $@

# Link: objects -> test binary
$(TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	@echo "[LD ] $@"
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(EXTRA_LDFLAGS)

# Include auto-generated header dependency files
-include $(DEP)

# Run the test binary.  EXEC_WRAPPER lets the same suite run on wasm32 -- `make run
# CC=emcc EXEC_WRAPPER=node` -- where size_t and long are 32 bits.  It is
# resolved through the shell because, with emsdk on PATH,
# /opt/emsdk/node is a directory that precedes /usr/bin and make's own exec
# stops at it with "Permission denied".
EXEC_WRAPPER ?=
EXEC_WRAPPER_BIN := $(if $(EXEC_WRAPPER),$(shell command -v $(EXEC_WRAPPER)))
RUN_ENV ?=
run: $(TARGET)
	$(RUN_ENV) $(EXEC_WRAPPER_BIN) $(TARGET)

# Clean this test's artifacts
clean:
	rm -f $(TARGET)
	rm -rf $(OBJ_DIR)
