# The recorder's Matroska writer and reader (src/core/record/mkv_mux.c,
# mkv_read.c), against each other.  See test.c.
#
# No emulator harness and no stubs: the muxer and reader depend on libc
# alone.  Files are written under tests/unit/build/.

TEST_NAME     := record_mkv
TEST_SRCS     := test.c
TEST_HARNESS  := none
EXTRA_SRCS    := ../../../../src/core/record/mkv_mux.c ../../../../src/core/record/mkv_read.c
INCLUDE_FLAGS  = -I$(UNIT_ROOT)/support -I$(EMU_ROOT)/core/record
EXTRA_CFLAGS   = -DTEST_OUT_DIR='"$(BUILD_DIR)"'
SANITIZE     ?= $(if $(filter emcc%,$(notdir $(firstword $(CC)))),,-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
RUN_ENV       := ASAN_OPTIONS=detect_leaks=1
include ../../common.mk
