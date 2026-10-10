# The sector tags a guest writes survive a checkpoint.  They live only in
# memory (image->tags, disk_write_tag), not in the delta, so image_checkpoint
# writes them and checkpoint_images.c reads them back.  The suite saves an
# image into an in-memory checkpoint (test.c replaces the checkpoint stubs)
# and restores it through the real mac_checkpoint_restore_one_image.

TEST_NAME    := image_checkpoint_tags
TEST_SRCS    := test.c
TEST_HARNESS := isolated
OMIT_STUBS   := checkpoint
EXTRA_SRCS   := ../../../../src/machines/runtime/checkpoint_images.c \
                ../../../../src/core/job/seam.c \
                ../../../../src/core/storage/image.c \
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
                ../../../../src/core/object/value_format.c \
                ../../../../src/core/storage/drive_catalog.c \
                ../../../../src/core/storage/image_part.c \
                ../../../../src/core/storage/image_iso9660.c
EXTRA_CFLAGS  = -I$(EMU_ROOT)/machines/runtime
USE_SOURCE_LAYER := 1
include ../../common.mk
