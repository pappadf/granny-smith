# Lisa intelligent floppy controller unit test.
# Pins the per-command status-byte contract (a control command reports its own
# OK status, never a stale media-read error) — the regression for the spurious
# LOS "not in standard format" alert when booting the ProFile with no floppy.

TEST_NAME    := lisa_fdc
TEST_SRCS    := test.c
TEST_HARNESS := isolated
# Real lisa_fdc.c plus the image subsystem it links against for the disk_*/
# image_get_filename accessors (referenced but not reached on the empty-drive
# paths under test).  fdc_stubs.c supplies the GCR-geometry + guest-PC symbols
# that belong to subsystems this isolated suite deliberately does not link.
EXTRA_SRCS   := ../../../../src/machines/lisa/lisa_fdc.c \
                ../../../../src/core/storage/image.c \
                ../../../../src/core/storage/storage.c \
                ../../../../src/core/storage/appledouble.c \
                ../../../../src/core/storage/image_ndif.c \
                ../../../../src/core/storage/adc.c \
                ../../../../src/core/storage/resource_fork.c \
                ../../../../src/core/storage/macroman.c \
                ../../../../src/core/storage/rsrc_dcmp.c \
                fdc_stubs.c
# lisa_fdc.h lives under machines/lisa, which is not on common.mk's default path.
EXTRA_CFLAGS  = -I$(EMU_ROOT)/machines/lisa

include ../../common.mk
