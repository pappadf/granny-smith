# The image VFS backend (src/core/vfs/image_vfs.c), tested over in-memory HFS+
# volumes built by support/hfsplus_builder.c.  Until this suite image_vfs.c
# had no unit coverage: suites/vfs stubs it out.
#
# Volumes are built in memory and written to temp files, then mounted like
# any file.  The one image.c entry point image_vfs uses,
# image_key_is_open_writable, is stubbed in test.c and answers as each test
# sets it.  Everything else -- the namespaces, the HFS/UFS/APM parsers, the
# format registry, the resource-fork parser and decompressor -- is the real
# code.
#
# Natively it runs under ASan + UBSan, with a 256 MB allocation ceiling: one
# defect this suite pins is an allocation sized from an unchecked catalog
# field, and the ceiling is how a test sees the attempt.

TEST_NAME := image_vfs
TEST_SRCS := test.c
TEST_HARNESS := isolated
S := ../../../../src/core
EXTRA_SRCS := $(S)/vfs/image_vfs.c \
              $(S)/vfs/namespace.c \
              $(S)/vfs/ns_disk.c \
              $(S)/vfs/ns_archive.c \
              $(S)/storage/image_hfs.c \
              $(S)/storage/image_ufs.c \
              $(S)/storage/image_mfs.c \
              $(S)/storage/image_iso9660.c \
              $(S)/storage/image_part.c \
              $(S)/storage/image_apm.c \
              $(S)/storage/image_apm_io.c \
              ../../support/hfsplus_builder.c
USE_SOURCE_LAYER := 1

# Natively, realpath() is wrapped (test.c) so a test can make it fail the way
# a filesystem backend that cannot answer it does.  The wasm build runs
# without the wrap, and without that test.
ifeq ($(filter emcc%,$(notdir $(firstword $(CC)))),)
EXTRA_LDFLAGS += -Wl,--wrap=realpath
WRAP_CFLAGS := -DTEST_WRAP_REALPATH
endif

SANITIZE := $(if $(filter emcc%,$(notdir $(firstword $(CC)))),,-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
EXTRA_CFLAGS := $(SANITIZE) $(WRAP_CFLAGS)
LDFLAGS += $(SANITIZE)
export ASAN_OPTIONS := max_allocation_size_mb=256:detect_leaks=1

include ../../common.mk
