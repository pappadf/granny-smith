# Declaration-ROM builder unit test (runtime-vrom proposal stages B/C).
# Links the real declrom.c + gsvrom_data.c (runtime generation over the
# build-assembled 68K fragments); generates every personality's image
# host-side, validates it structurally, and exercises the builder's
# validation guards — no guest boot required.

TEST_NAME    := declrom
TEST_SRCS    := test.c
TEST_HARNESS := isolated
EXTRA_SRCS   := ../../../../src/core/peripherals/nubus/declrom.c \
                ../../../../src/core/peripherals/nubus/gsvrom_data.c
EXTRA_CFLAGS := -I$(abspath ../../../..)/build/vrom68k

include ../../common.mk

# gsvrom_data.c embeds the generated fragments header; delegate its
# production to the main build (vrom68k.mk — requires m68k binutils).
VROM68K_HEADER := $(WORKSPACE_ROOT)/build/vrom68k/gsvrom_fragments.h
$(VROM68K_HEADER):
	$(MAKE) -C $(WORKSPACE_ROOT) -f Makefile.headless build/vrom68k/gsvrom_fragments.h
$(OBJ_DIR)/src/core/peripherals/nubus/gsvrom_data.o: $(VROM68K_HEADER)
