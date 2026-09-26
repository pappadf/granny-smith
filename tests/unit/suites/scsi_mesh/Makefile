# The MESH controller.
#
# MESH lived under machines/tnt/ with no handle of its own, taking config_t*
# everywhere and reaching its state through tnt_st(cfg)->mesh.  It could not be
# constructed outside a whole Power Macintosh, so it had no unit tests at all --
# the only controller in the subsystem without them, and the only one whose bugs
# had to be found through a full boot.
#
# This suite is the reason the move was worth doing.
TEST_NAME    := scsi_mesh
TEST_SRCS    := test.c
# The test supplies the mock bus, the mock guest memory and a recording
# checkpoint stream itself; from support/ it takes only the assert stub.
TEST_HARNESS := none
STUBS        := assert
# scsi_msgsession.c is the real IDENTIFY/SDTR/WDTR parser, shared with the
# 53C8xx SCRIPTS engine: pure logic, no emulator dependencies, so the suite
# links the actual code rather than a stub.
EXTRA_SRCS   := ../../../../src/core/peripherals/scsi.c \
                ../../../../src/core/peripherals/scsi_bus.c \
                ../../../../src/core/peripherals/scsi_cdrom.c \
                ../../../../src/core/peripherals/scsi_mesh.c \
                ../../../../src/core/peripherals/scsi_msgsession.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/object/api.c \
                ../../../../src/core/object/value_format.c
include ../../common.mk
