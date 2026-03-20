# Makefile for the standalone disasm tool
# Builds a 68000/68030 disassembler reusing the core cpu_disasm.c decoder.
# Core sources are compiled directly from src/ (not copied), following
# the same pattern as tests/unit/ with a force-included platform override.

CC      ?= gcc
CFLAGS  += -Wall -Wextra -O2

TOOL    := disasm

# Core source directories
CORE_CPU_DIR := ../../src/core/cpu
CORE_DBG_DIR := ../../src/core/debug
CORE_DIR     := ../../src/core

# Directory of this Makefile (tools/disasm/)
TOOL_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# Include paths for core headers (cpu.h, common.h, debug_mac.h, cpu_decode.h)
# Force-include our platform override before all sources to prevent
# the real platform.h from loading (same pattern as tests/unit/support/).
INCLUDES := -I$(CORE_DIR) -I$(CORE_CPU_DIR) -I$(CORE_DBG_DIR) \
            -I$(TOOL_DIR) \
            -include $(TOOL_DIR)/platform.h

# Tool-specific sources (local to this directory)
LOCAL_SRCS := disasm.c trap_lookup.c stubs.c

# Core sources compiled directly from src/ (not copied)
CORE_SRCS := $(CORE_CPU_DIR)/cpu_disasm.c $(CORE_DBG_DIR)/mac_traps_data.c

.PHONY: all clean

all: $(TOOL)

$(TOOL): $(LOCAL_SRCS) $(CORE_SRCS) platform.h trap_lookup.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(LOCAL_SRCS) $(CORE_SRCS)

clean:
	rm -f $(TOOL)
