# SCSI transfer-range bounds, for READ and WRITE on both CDB lengths.
#
# 03-scsi F-02: WRITE had no bounds check at all — only two assert()s in
# command_complete, and -DNDEBUG removes those from the release wasm profile,
# so an out-of-range WRITE reached disk_write_data (which drops the unbacked
# tail) and the SCSI layer then reported STATUS GOOD.  Silent data loss
# reported as success.  F-04: the READ check multiplied in size_t, 32 bits on
# wasm32, so a large LBA wrapped and the wrong blocks were served as valid.
#
# Drives real CDBs through the real scsi.c against a real image, via the same
# external-initiator API the 53C96 bus master uses.  No emulator, no ROM, no
# media from gs-test-data.

TEST_NAME    := scsi_bounds
TEST_SRCS    := test.c
TEST_HARNESS := isolated
EXTRA_SRCS   := ../../../../src/core/peripherals/scsi.c \
                ../../../../src/core/peripherals/scsi_cdrom.c \
                ../../../../src/core/storage/image.c \
                ../../../../src/core/storage/storage.c \
                ../../../../src/core/storage/appledouble.c \
                ../../../../src/core/storage/image_ndif.c \
                ../../../../src/core/storage/image_udif.c \
                ../../../../src/core/storage/inflate.c \
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
