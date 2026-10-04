# A writable disk keeps its writes across close and reopen
# (src/core/storage/image.c): a machine that is replaced closes its disks, and
# the next machine.boot mounts the same files again.

TEST_NAME    := image_persist
TEST_SRCS    := test.c
TEST_HARNESS := isolated
EXTRA_SRCS   := ../../../../src/core/job/seam.c \
                ../../../../src/core/event/gs_event.c \
                ../../../../src/core/storage/image.c \
                ../../../../src/core/storage/storage_util.c \
                ../../../../src/core/storage/image_scratch.c \
                ../../../../src/core/storage/storage.c \
                ../../../../src/peeler/lib/appledouble.c \
                ../../../../src/core/storage/image_ndif.c \
                ../../../../src/core/crc32.c \
                ../../../../src/core/storage/image_udif.c \
                ../../../../src/core/storage/inflate.c \
                ../../../../src/core/storage/adc.c \
                ../../../../src/core/storage/resource_fork.c \
                ../../../../src/core/storage/rsrc_dcmp.c \
                ../../../../src/core/storage/macroman.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/object/api.c \
                ../../../../src/core/object/value_format.c \
                ../../../../src/core/storage/drive_catalog.c \
                ../../../../src/core/storage/image_part.c \
                ../../../../src/core/storage/image_iso9660.c
USE_SOURCE_LAYER := 1
include ../../common.mk
