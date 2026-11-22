# A GCR floppy's sector tags are in-band (each sector one [12 tag][512 data]
# block), so they live in the delta and travel with the data: through a
# checkpoint and through reopening the delta.  The suite saves an image into
# an in-memory checkpoint (test.c replaces the checkpoint stubs) and restores
# it through the real mac_checkpoint_restore_one_image.

TEST_NAME    := image_tags
TEST_SRCS    := test.c
TEST_HARNESS := isolated
OMIT_STUBS   := checkpoint
EXTRA_SRCS   := ../../../../src/machines/runtime/checkpoint_images.c \
                ../../../../src/core/job/seam.c \
                ../../../../src/core/storage/image.c \
                ../../../../src/core/storage/image_kind.c \
                ../../../../src/core/platform_hooks.c \
                ../../../../src/core/storage/image_access.c \
                ../../../../src/core/storage/image_wrap_sniff.c \
                ../../../../src/core/storage/image_apm.c \
                ../../../../src/core/storage/storage.c \
                ../../../../src/peeler/lib/appledouble.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/object/api.c \
                ../../../../src/core/object/json_value.c \
                ../../../../src/core/object/value_format.c ../../../../src/core/vbuf.c \
                ../../../../src/core/storage/drive_catalog.c \
                ../../../../src/core/storage/image_part.c \
                ../../../../src/core/storage/image_iso9660.c
EXTRA_CFLAGS  = -I$(EMU_ROOT)/machines/runtime
USE_SOURCE_LAYER := 1
include ../../common.mk
