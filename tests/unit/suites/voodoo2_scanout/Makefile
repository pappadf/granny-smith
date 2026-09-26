# The Voodoo2's scanout raster against the largest videoDimensions a guest
# can program (src/core/peripherals/pci/cards/voodoo2.c).  Links the real
# card, its rasterisers and the value layer; test.c stubs the PCI bus, the
# object tree and the scheduler.  Natively under ASan + UBSan: the defect
# this pins was a host heap write past the scanout allocation, which
# nothing short of a sanitizer sees.

TEST_NAME := voodoo2_scanout

UNIT_ROOT      := $(abspath ../../)
WORKSPACE_ROOT := $(abspath $(UNIT_ROOT)/../..)
EMU_ROOT       := $(WORKSPACE_ROOT)/src

BUILD_DIR := $(UNIT_ROOT)/build
OBJ_DIR   := $(BUILD_DIR)/obj/$(TEST_NAME)
TARGET    := $(BUILD_DIR)/$(TEST_NAME)

CC ?= gcc
SANITIZE := $(if $(filter emcc%,$(notdir $(firstword $(CC)))),,-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
BASE_CFLAGS := -O0 -g -Wall -Wextra $(SANITIZE)
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
LDFLAGS := $(SANITIZE) -lpthread -lm
export ASAN_OPTIONS := detect_leaks=1

SRCS := $(CURDIR)/test.c \
        $(EMU_ROOT)/core/peripherals/pci/cards/voodoo2.c \
        $(EMU_ROOT)/core/peripherals/pci/cards/voodoo2_raster.c \
        $(EMU_ROOT)/core/peripherals/pci/cards/voodoo2_gpu.c \
        $(EMU_ROOT)/core/object/value.c \
        $(UNIT_ROOT)/support/stub_checkpoint.c \
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
