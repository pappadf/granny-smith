# A MOOF's bit-level capture served to the IWM / SWIM GCR read path
# (src/core/storage/image_capture.c and floppy_media_track in
# src/core/peripherals/floppy_gcr.c): the content rule that decides between
# the captured track, a splice of changed sectors into it and a track laid
# down from the sectors; unreadable sectors; the write-protect flag; a
# .moof export copying unchanged tracks through; and the drive's write-back
# of a captured track at an eject (and the write-through), which must read
# it as a loop and write only its own track's sectors, once each; and weak
# bits -- a track with long runs of no flux transition is redrawn with fresh,
# reproducible noise each time the head passes the index.
#
# Captures are made by the MOOF writer (moof_encode) and then edited bit by
# bit the way protected disks differ from a formatter's output -- a $DE in
# front of one data mark, a data field that is not a sector, a field with no
# bit-slip marks, headers naming another track or sector, a revolution that
# starts inside a sector as a real capture's may -- so the suite
# needs no fixtures and no copyrighted media.  The real image layer and
# storage engine open them from temp files; floppy_gcr.c is #included, so
# its static splice can be driven directly, with the drive side stubbed.

TEST_NAME    := moof_capture
TEST_SRCS    := test.c
TEST_HARNESS := isolated
EXTRA_SRCS   := ../../../../src/core/job/seam.c \
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
                ../../../../src/core/object/json_value.c \
                ../../../../src/core/object/value_format.c ../../../../src/core/vbuf.c \
                ../../../../src/core/storage/drive_catalog.c \
                ../../../../src/core/storage/image_part.c \
                ../../../../src/core/storage/image_iso9660.c
EXTRA_CFLAGS  = -I$(EMU_ROOT)/core/peripherals
USE_SOURCE_LAYER := 1
include ../../common.mk
