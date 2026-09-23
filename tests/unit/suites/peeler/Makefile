# The peeler archive library, tested as an untrusted parser.  See test.c.
#
# Deliberately NOT common.mk: peeler is a self-contained library -- nothing in
# src/peeler/lib includes anything from src/core -- so it needs no emulator
# harness and no stubs, and linking one would only hide an accidental
# dependency if one were ever added.
#
# test.c #includes formats/sit15.c so that its arithmetic *encoder* can drive
# the decoder's own model code (model_setup/model_bump), which makes the two
# agree by construction.  sit15.c is therefore compiled as part of test.c and
# left out of PEELER_SRCS below, or its one external symbol would be defined
# twice.
#
# CC and EXEC_WRAPPER are overridable so the same suite runs on wasm32, where
# size_t is 32 bits (09-WORK-ORDER.md §0.2):
#   make run CC=emcc EXEC_WRAPPER=node TARGET_EXT=.js

TEST_NAME := peeler

UNIT_ROOT      := $(abspath ../../)
WORKSPACE_ROOT := $(abspath $(UNIT_ROOT)/../..)
PEELER_DIR     := $(WORKSPACE_ROOT)/src/peeler

TARGET_EXT ?=
BUILD_DIR  ?= $(UNIT_ROOT)/build
OBJ_DIR    := $(BUILD_DIR)/obj/$(TEST_NAME)$(TARGET_EXT)
TARGET     := $(BUILD_DIR)/$(TEST_NAME)$(TARGET_EXT)

CC     ?= gcc
EXEC_WRAPPER ?=
# Resolve the runner through the shell, not make's own exec: with emsdk on PATH,
# /opt/emsdk/node is a *directory* that precedes /usr/bin, and execvp stops at
# it with "Permission denied" where a shell lookup skips it.
EXEC_WRAPPER_BIN := $(if $(EXEC_WRAPPER),$(shell command -v $(EXEC_WRAPPER)))
CFLAGS := -O0 -g -Wall -Wextra -I$(UNIT_ROOT)/support -I$(PEELER_DIR)/include -I$(PEELER_DIR)/lib
LDFLAGS ?=

PEELER_SRCS := $(PEELER_DIR)/lib/peeler.c \
               $(PEELER_DIR)/lib/err.c \
               $(PEELER_DIR)/lib/util.c \
               $(PEELER_DIR)/lib/formats/bin.c \
               $(PEELER_DIR)/lib/formats/cpt.c \
               $(PEELER_DIR)/lib/formats/hqx.c \
               $(PEELER_DIR)/lib/formats/sit.c \
               $(PEELER_DIR)/lib/formats/sit3.c \
               $(PEELER_DIR)/lib/formats/sit13.c

SRCS := $(CURDIR)/test.c $(PEELER_SRCS)

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
	@$(EXEC_WRAPPER_BIN) $(TARGET)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

-include $(DEP)
