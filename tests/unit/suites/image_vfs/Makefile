# The image VFS backend (src/core/vfs/image_vfs.c), tested over in-memory HFS+
# volumes built by support/hfsplus_builder.c.  Until this suite image_vfs.c
# had no unit coverage: suites/vfs stubs it out.
#
# The image.c entry points image_vfs uses are stubbed in test.c: four serve
# the in-memory volume, and image_path_is_open_writable answers as each test
# sets it.  Everything else -- the HFS/UFS/APM/NDIF parsers, the resource-fork
# parser and decompressor -- is the real code.
#
# Natively it runs under ASan + UBSan, with a 256 MB allocation ceiling: one
# defect this suite pins is an allocation sized from an unchecked catalog
# field, and the ceiling is how a test sees the attempt.

TEST_NAME := image_vfs
TEST_SRCS := test.c
TEST_HARNESS := isolated
S := ../../../../src/core
EXTRA_SRCS := $(S)/vfs/image_vfs.c \
              $(S)/storage/image_hfs.c \
              $(S)/storage/image_ufs.c \
              $(S)/storage/image_part.c \
              $(S)/storage/image_scratch.c \
              $(S)/storage/storage_util.c \
              $(S)/storage/image_apm.c \
              $(S)/storage/image_apm_io.c \
              $(S)/storage/image_ndif.c \
              $(S)/storage/adc.c \
              $(S)/storage/resource_fork.c \
              $(S)/storage/rsrc_dcmp.c \
              $(S)/storage/macroman.c \
              ../../support/hfsplus_builder.c

SANITIZE := $(if $(filter emcc%,$(notdir $(firstword $(CC)))),,-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
EXTRA_CFLAGS := $(SANITIZE)
LDFLAGS += $(SANITIZE)
export ASAN_OPTIONS := max_allocation_size_mb=256:detect_leaks=1

include ../../common.mk
