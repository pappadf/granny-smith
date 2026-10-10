# A disk whose backing image fails a read it should serve (a corrupt
# compressed chunk, a bad archive member, a host I/O error), end to end:
# the image layer reports it without halting, the SCSI target turns it into
# CHECK CONDITION / MEDIUM ERROR, and an export or consolidated checkpoint of
# the disk fails rather than embedding zeros.  Built as the debug profile is
# (asserts on), unlike scsi_short_read, so a halt on the failure is a test
# failure here.
#
# The checkpoint stub is replaced by an in-memory stream in test.c, so a
# checkpoint's error flag and a crafted snapshot header can be observed.

TEST_NAME    := storage_read_error
TEST_SRCS    := test.c
TEST_HARNESS := isolated
OMIT_STUBS   := checkpoint
S := ../../../../src/core
EXTRA_SRCS   := $(S)/job/seam.c \
                $(S)/peripherals/scsi.c \
                $(S)/peripherals/scsi_bus.c \
                $(S)/event/gs_event.c \
                $(S)/peripherals/scsi_cdrom.c \
                $(S)/storage/image.c \
                $(S)/storage/image_wrap_sniff.c \
                $(S)/storage/image_apm.c \
                $(S)/storage/image_scratch.c \
                $(S)/storage/storage.c \
                ../../../../src/peeler/lib/appledouble.c \
                $(S)/storage/image_part.c \
                $(S)/storage/image_iso9660.c \
                $(S)/storage/image_access.c \
                $(S)/object/object.c \
                $(S)/object/parse.c \
                $(S)/object/value.c \
                $(S)/object/meta.c \
                $(S)/object/api.c \
                $(S)/object/value_format.c \
                $(S)/storage/drive_catalog.c
USE_SOURCE_LAYER := 1

SANITIZE := $(if $(filter emcc%,$(notdir $(firstword $(CC)))),,-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
EXTRA_CFLAGS := $(SANITIZE)
LDFLAGS += $(SANITIZE)

include ../../common.mk
