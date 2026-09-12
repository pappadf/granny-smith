# Guest-reachable NCR 5380 register transitions must decline, not abort.
#
# 03-scsi F-07: the bus state machine guarded transitions a guest fully
# controls through the register file with plain assert().  The default headless
# build keeps assertions live (Makefile.headless: "No -DNDEBUG: keep assertions
# active in the headless test/debug tool"), so five byte-writes to one register
# aborted the emulator -- and every CI integration row runs that build.  The
# GS_FAST/NDEBUG profiles compiled the same checks out and continued into a
# state the rest of the file assumes is impossible.
#
# Drives write_uint8 directly through scsi->memory_interface -- the exact entry
# point the guest's store instruction lands on -- so no memory map, machine or
# ROM is needed.

TEST_NAME    := scsi_registers
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
