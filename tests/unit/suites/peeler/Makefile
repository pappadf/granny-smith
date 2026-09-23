# The peeler archive library, tested as an untrusted parser.  See test.c.
#
# Deliberately NOT common.mk: peeler is a self-contained library -- nothing in
# src/peeler/lib includes anything from src/core -- so it needs no emulator
# harness and no stubs, and linking one would only hide an accidental
# dependency if one were ever added.
#
# test.c #includes formats/sit15.c so that its arithmetic *encoder* can drive
# the decoder's own model code (model_setup/model_bump), and formats/sit13.c
# so that its bit writer uses the decoder's own meta-code tables -- encoder and
# decoder agree by construction.  Both are therefore compiled as part of
# test.c and left out of PEELER_SRCS below, or their external symbols would be
# defined twice.
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
# Native builds run under ASan + UBSan, and UB aborts rather than printing and
# carrying on.  Several peeler defects are small heap overruns (two bytes past
# a malloc, F-02) that land in allocator padding and do not crash natively; a
# test of one must fail on the unfixed code, and without a sanitizer it would
# pass for the wrong reason.  Not under emcc: the wasm32 run is for 32-bit
# arithmetic, and ASan there costs more than it finds.
SANITIZE ?= $(if $(filter emcc%,$(notdir $(firstword $(CC)))),,-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
CFLAGS  := -O0 -g -Wall -Wextra $(SANITIZE) -I$(UNIT_ROOT)/support -I$(PEELER_DIR)/include -I$(PEELER_DIR)/lib
LDFLAGS ?=
LDFLAGS += $(SANITIZE)

PEELER_SRCS := $(PEELER_DIR)/lib/peeler.c \
               $(PEELER_DIR)/lib/appledouble.c \
               $(PEELER_DIR)/lib/err.c \
               $(PEELER_DIR)/lib/util.c \
               $(PEELER_DIR)/lib/formats/bin.c \
               $(PEELER_DIR)/lib/formats/cpt.c \
               $(PEELER_DIR)/lib/formats/hqx.c \
               $(PEELER_DIR)/lib/formats/sit.c \
               $(PEELER_DIR)/lib/formats/sit3.c

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

# Leak detection is on: every decoder's allocations are owned by its decode
# context and freed on abort (09 A2), and LeakSanitizer is how a new abort
# path that leaks gets caught -- F-10, F-11 and F-12 were exactly that.
DETECT_LEAKS ?= 1
run: $(TARGET)
	@ASAN_OPTIONS=detect_leaks=$(DETECT_LEAKS) $(EXEC_WRAPPER_BIN) $(TARGET)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

-include $(DEP)
