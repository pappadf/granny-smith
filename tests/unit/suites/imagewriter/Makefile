# The ImageWriter: the command interpreter, the sheet raster and the PDF
# writer, driven directly (no machine, no serial port).  See test.c.
TEST_NAME    := imagewriter
TEST_SRCS    := test.c
TEST_HARNESS := isolated
EXTRA_SRCS   := ../../../../src/core/printer/iw_interp.c \
                ../../../../src/core/printer/iw_page.c \
                ../../../../src/core/printer/iw_font.c \
                ../../../../src/core/printer/iw_font_data.c \
                ../../../../src/core/printer/pdf_writer.c \
                ../../../../src/core/storage/deflate.c \
                ../../../../src/core/storage/inflate.c \
                ../../../../src/peeler/lib/inflate.c
include ../../common.mk
