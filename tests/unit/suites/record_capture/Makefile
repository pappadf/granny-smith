# The recorder's machine-free parts (src/core/record): the audio timeline
# (record_audio.c), the pixel work (record_pixels.c), and the coder and
# worker (record_thread.c) driven record by record through the ring, inline
# and on a thread, into a Matroska file that is read back and decoded.
# See test.c.
#
# No emulator harness: those files depend on libc, pthreads, the codec
# library, the muxer and the mailbox ring alone.  Files are written under
# tests/unit/build/.

TEST_NAME     := record_capture
TEST_SRCS     := test.c
TEST_HARNESS  := none
STUBS         := assert
EXTRA_SRCS    := ../../../../src/core/record/record_audio.c ../../../../src/core/record/record_pixels.c \
                 ../../../../src/core/record/record_thread.c ../../../../src/core/record/mkv_mux.c \
                 ../../../../src/core/record/mkv_read.c ../../../../src/core/mailbox/mailbox_ring.c \
                 $(wildcard ../../../../src/codec/lib/*.c)
EXTRA_CFLAGS   = -DTEST_OUT_DIR='"$(BUILD_DIR)"'
EXTRA_LDFLAGS := -lpthread
SANITIZE     ?= $(if $(filter emcc%,$(notdir $(firstword $(CC)))),,-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
RUN_ENV       := ASAN_OPTIONS=detect_leaks=1
include ../../common.mk
