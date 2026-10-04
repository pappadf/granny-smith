# Where a writable mount's delta+journal land when the caller names no delta
# directory (src/core/storage/image.c, image_create): the scratch root --
# GS_STORAGE_CACHE when set, else /tmp/gs-image-ro -- and never beside the
# base.  Deltas are per-instance, so one beside the base is an orphan the
# moment the process exits, and shared media (tests/data) collected them from
# every ad-hoc debug session.

TEST_NAME    := image_sidecars
TEST_SRCS    := test.c
TEST_HARNESS := isolated
EXTRA_SRCS   := ../../../../src/core/job/seam.c \
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
