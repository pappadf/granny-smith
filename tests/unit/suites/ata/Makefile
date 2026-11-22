# The ATA channel (core/peripherals/ata.c): the task file, the PIO and DMA
# data paths of an ATA hard disk over an in-memory medium, and an ATAPI
# CD-ROM whose PACKET commands run on the real SCSI CD-ROM model.
TEST_NAME    := ata
TEST_SRCS    := test.c
# The test supplies the medium, the checkpoint stream and the few emulator
# hooks the SCSI bus model reaches for; from support/ it takes only the
# assert stub.
TEST_HARNESS := none
STUBS        := assert checkpoint_strings
EXTRA_SRCS   := ../../../../src/core/job/seam.c \
                ../../../../src/core/peripherals/ata.c \
                ../../../../src/core/peripherals/scsi.c \
                ../../../../src/core/peripherals/scsi_bus.c \
                ../../../../src/core/event/event.c \
                ../../../../src/core/peripherals/scsi_cdrom.c \
                ../../../../src/core/peripherals/scsi_msgsession.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/object/api.c \
                ../../../../src/core/object/json_value.c \
                ../../../../src/core/object/value_format.c ../../../../src/core/vbuf.c
# scsi.c identifies CD-ROM contents through the format registry
# (image_source in image_part.c), as in the other SCSI suites.
EXTRA_SRCS += ../../../../src/core/storage/image_part.c
USE_SOURCE_LAYER := 1
EXTRA_SRCS += ../../../../src/core/storage/image_iso9660.c
# The real image_t accessors; the rest of the image layer is faked here
EXTRA_SRCS += ../../../../src/core/storage/image_access.c
EXTRA_SRCS += ../../../../src/core/storage/image_class.c
# The drive-to-image checkpoint reference the media controllers write
EXTRA_SRCS += ../../../../src/core/storage/image_ref.c
include ../../common.mk
