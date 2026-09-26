# A READ the backing store fails, in the build that ships.
#
# run_cmd's only check on disk_read_data's count was assert(), which -DNDEBUG
# removes from the release profile, so there a failed in-range read returned
# STATUS GOOD over whatever the data buffer last held.  In every other build
# image.c's GS_ASSERTF aborts first, so this suite is built as the release
# profile is (-DGS_FAST -DNDEBUG): both checks gone, and only the SCSI layer's
# own handling between the failure and the guest.

TEST_NAME    := scsi_short_read
TEST_SRCS    := test.c
TEST_HARNESS := isolated
EXTRA_CFLAGS := -DGS_FAST -DNDEBUG
EXTRA_SRCS   := ../../../../src/core/peripherals/scsi.c \
                ../../../../src/core/peripherals/scsi_bus.c \
                ../../../../src/core/peripherals/scsi_cdrom.c \
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
                ../../../../src/core/storage/drive_catalog.c
include ../../common.mk
