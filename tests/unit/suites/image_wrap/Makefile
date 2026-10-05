# The bare-volume wrapper's layout (src/core/storage/image_wrap.c): the
# synthesised Driver Descriptor Map and partition map are checked with the
# in-tree APM parser, the driver-partition checksum against the ROM's
# algorithm, the bare-volume sniff against partitioned and non-HFS heads, and
# the driverless-partitioned-disk sniff against disks with drivers, several or
# no HFS partitions, and A/UX partitions.
# No guest boot: tests/integration/scsi-bare-volume boots the result.

TEST_NAME    := image_wrap
TEST_SRCS    := test.c
TEST_HARNESS := isolated
EXTRA_SRCS   := ../../../../src/core/storage/image_wrap.c \
                ../../../../src/core/storage/image_wrap_sniff.c \
                ../../../../src/core/storage/image_apm.c
EXTRA_CFLAGS := -I$(abspath ../../../..)/build/gsdisk

include ../../common.mk

# image_wrap.c embeds the generated GSDisk driver header; delegate its
# production to the main build (gsdisk.mk — requires m68k binutils).
GSDISK_HEADER := $(WORKSPACE_ROOT)/build/gsdisk/gsdisk_driver.h
$(GSDISK_HEADER):
	$(MAKE) -C $(WORKSPACE_ROOT) -f Makefile.headless build/gsdisk/gsdisk_driver.h
$(OBJ_DIR)/src/core/storage/image_wrap.o: $(GSDISK_HEADER)
