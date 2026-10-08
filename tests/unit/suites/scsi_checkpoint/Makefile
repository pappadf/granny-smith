# SCSI checkpoint round-trip.
#
# The save was a plain-data prefix that stopped at `devices` plus a hand-written
# per-field loop, and the loop forgot fields.  sense, prevent_removal and
# default_block_size were all silently dropped, so a checkpoint taken between a
# CHECK CONDITION and the driver's REQUEST SENSE restored as NO SENSE --
# measured happening twice in suite-quadra, once with UNIT ATTENTION / 0x29
# pending.
#
# The fix moves the image pointers out of the device records so the whole
# plain-data region saves in one write, the way via_t, scc_t and rtc_t do.
# This suite exists so the NEXT field added to that region is caught if it
# does not survive: it stages state across the whole struct, round-trips it,
# and compares.

TEST_NAME    := scsi_checkpoint
TEST_SRCS    := test.c
TEST_HARNESS := none
# The test supplies a recording checkpoint stream and its own link stubs;
# from support/ it takes only the assert stub.
STUBS        := assert
EXTRA_SRCS   := ../../../../src/core/job/seam.c \
              ../../../../src/core/peripherals/scsi.c \
                ../../../../src/core/peripherals/scsi_bus.c \
                ../../../../src/core/event/gs_event.c \
                ../../../../src/core/peripherals/scsi_cdrom.c \
                ../../../../src/core/peripherals/scsi_53c96.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/object/api.c \
                ../../../../src/core/object/value_format.c
EXTRA_SRCS += ../../../../src/core/storage/image_part.c
USE_SOURCE_LAYER := 1
EXTRA_SRCS += ../../../../src/core/storage/image_iso9660.c
# The real image_t accessors; the rest of the image layer is faked here
EXTRA_SRCS += ../../../../src/core/storage/image_access.c
include ../../common.mk
