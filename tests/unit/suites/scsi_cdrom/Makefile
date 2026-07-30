# CD-ROM transfer-size unit test.
#
# Drives the REAL target stack (scsi.c + scsi_cdrom.c over a temp image)
# through the external-initiator API — scsi_external_select, then CDB bytes via
# scsi_push_data_out_byte — which is the same entry the 53C96 bus master uses.
# No emulator, no ROM, no media from gs-test-data.

TEST_NAME    := scsi_cdrom
TEST_SRCS    := test.c
TEST_HARNESS := isolated
EXTRA_SRCS   := ../../../../src/core/peripherals/scsi.c \
                ../../../../src/core/peripherals/scsi_cdrom.c \
                ../../../../src/core/storage/image.c \
                ../../../../src/core/storage/storage.c \
                ../../../../src/core/storage/appledouble.c \
                ../../../../src/core/storage/image_ndif.c \
                ../../../../src/core/storage/adc.c \
                ../../../../src/core/storage/resource_fork.c \
                ../../../../src/core/storage/rsrc_dcmp.c \
                ../../../../src/core/storage/macroman.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/object/api.c \
                ../../../../src/core/storage/drive_catalog.c
include ../../common.mk
