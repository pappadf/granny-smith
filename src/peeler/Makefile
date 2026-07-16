# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# Makefile — Build system for libpeeler.
#
# Targets:
#   all           Build static library and CLI (default)
#   test          Run the full test suite
#   clean         Remove build artifacts
#
# Usage:
#   make              # build library + CLI
#   make test         # build + run tests
#   make clean        # remove build/

# ============================================================================
# Configuration
# ============================================================================

CC       ?= cc
AR       ?= ar
ARFLAGS   = rcs

CFLAGS   ?= -Wall -Wextra -Wpedantic -Werror
CFLAGS   += -std=c99

BUILD     = build
LIB_DIR   = $(BUILD)/lib
CMD_DIR   = $(BUILD)/cmd
FMT_DIR   = $(BUILD)/lib/formats

# ============================================================================
# Sources
# ============================================================================

LIB_SRCS  = lib/err.c      \
            lib/util.c     \
            lib/peeler.c

FMT_SRCS  = lib/formats/hqx.c   \
            lib/formats/bin.c    \
            lib/formats/sit.c    \
            lib/formats/sit3.c   \
            lib/formats/sit13.c  \
            lib/formats/sit15.c  \
            lib/formats/cpt.c

CMD_SRCS  = cmd/main.c

LIB_OBJS  = $(patsubst %.c,$(BUILD)/%.o,$(LIB_SRCS) $(FMT_SRCS))
CMD_OBJS  = $(patsubst %.c,$(BUILD)/%.o,$(CMD_SRCS))

LIB_OUT   = $(BUILD)/libpeeler.a
CLI_OUT   = $(BUILD)/peeler

# Include paths: public header for CLI, private lib dir for format sources
LIB_CFLAGS = -Iinclude -Ilib
CMD_CFLAGS = -Iinclude

# ============================================================================
# Default Target
# ============================================================================

.PHONY: all
all: $(CLI_OUT)

# ============================================================================
# Static Library
# ============================================================================

$(LIB_OUT): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) $(ARFLAGS) $@ $^

# Library object files
$(BUILD)/lib/%.o: lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIB_CFLAGS) -c -o $@ $<

# Format decoder object files (same flags as library)
$(BUILD)/lib/formats/%.o: lib/formats/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIB_CFLAGS) -c -o $@ $<

# ============================================================================
# CLI
# ============================================================================

$(CLI_OUT): $(CMD_OBJS) $(LIB_OUT)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(CMD_OBJS) $(LIB_OUT)

$(BUILD)/cmd/%.o: cmd/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CMD_CFLAGS) -c -o $@ $<

# ============================================================================
# Tests
# ============================================================================

.PHONY: test
test: $(CLI_OUT)
	@rc=0; \
	./test/run_tests.sh --peeler $(CLI_OUT) --test-dir test/testfiles || rc=1; \
	if [ -d test/internal_testfiles ]; then \
	    ./test/run_tests.sh --peeler $(CLI_OUT) --test-dir test/internal_testfiles || rc=1; \
	fi; \
	exit $$rc

# ============================================================================
# Clean
# ============================================================================

.PHONY: clean
clean:
	rm -rf $(BUILD)
