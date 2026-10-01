# The ATA channel (core/peripherals/ata.c): the task file, the PIO and DMA
# data paths of an ATA hard disk over an in-memory medium, and an ATAPI
# CD-ROM whose PACKET commands run on the real SCSI CD-ROM model.
TEST_NAME    := ata
TEST_SRCS    := test.c
# The test supplies the medium, the checkpoint stream and the few emulator
# hooks the SCSI bus model reaches for; from support/ it takes only the
# assert stub.
TEST_HARNESS := none
STUBS        := assert
EXTRA_SRCS   := ../../../../src/core/job/seam.c \
                ../../../../src/core/peripherals/ata.c \
                ../../../../src/core/peripherals/scsi.c \
                ../../../../src/core/peripherals/scsi_bus.c \
                ../../../../src/core/event/gs_event.c \
                ../../../../src/core/peripherals/scsi_cdrom.c \
                ../../../../src/core/peripherals/scsi_msgsession.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/object/api.c \
                ../../../../src/core/object/value_format.c
include ../../common.mk
