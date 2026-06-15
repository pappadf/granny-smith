# Makefile for the standalone dump tool
# Builds a reverse-engineering tool that extracts and disassembles classic
# Mac forked files and A/UX COFF binaries.  Core sources are compiled
# directly from src/ (not copied), following the same pattern as
# tools/disasm/ with a force-included platform override.

CC      ?= gcc
CFLAGS  += -Wall -Wextra -O2

TOOL    := dump

# Source directories
CORE_DIR         := ../../src/core
CORE_CPU_DIR     := $(CORE_DIR)/cpu
CORE_DBG_DIR     := $(CORE_DIR)/debug
CORE_STORAGE_DIR := $(CORE_DIR)/storage

# Directory of this Makefile (tools/dump/)
TOOL_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# Include paths.  Force-include the platform override before all sources
# to prevent the real emulator platform.h from loading.
INCLUDES := -I$(CORE_DIR) -I$(CORE_CPU_DIR) -I$(CORE_DBG_DIR) -I$(CORE_STORAGE_DIR) \
            -I$(TOOL_DIR) \
            -include $(TOOL_DIR)/platform.h

# Tool-local sources.  Everything under tools/dump/ except platform.h.
LOCAL_SRCS := dump.c \
              annotate_disasm.c code_segment.c symbols.c \
              coff.c coff_dump.c aux_syscalls.c \
              decoders/decoders.c \
              decoders/decode_bndl.c decoders/decode_dialog.c decoders/decode_menu.c \
              decoders/decode_size.c decoders/decode_str.c decoders/decode_vers.c \
              stubs.c

# Core sources compiled directly from src/ (not copied).  The dump tool
# uses the same resource-fork parser and decompressors as the emulator —
# both halves of the boundary that just landed in PR #43.
CORE_SRCS := $(CORE_CPU_DIR)/cpu_disasm.c \
             $(CORE_DBG_DIR)/mac_traps_data.c \
             $(CORE_DBG_DIR)/mac_globals_data.c \
             $(CORE_STORAGE_DIR)/resource_fork.c \
             $(CORE_STORAGE_DIR)/rsrc_dcmp.c \
             $(CORE_STORAGE_DIR)/macroman.c

.PHONY: all clean

all: $(TOOL)

$(TOOL): $(LOCAL_SRCS) $(CORE_SRCS) platform.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(LOCAL_SRCS) $(CORE_SRCS)

clean:
	rm -f $(TOOL)
