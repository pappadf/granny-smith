# The codec library (src/codec), tested as an untrusted parser.  See test.c.
#
# No emulator harness and no stubs, and only the codec's own header on the
# include path: the library depends on nothing but libc, and compiling
# against the core headers would only hide an accidental dependency.
#
# vectors.h is generated: python3 make_vectors.py > vectors.h (needs the
# pyliblzfse package for its LZFSE encoder).
#
# The same suite runs on wasm32, where size_t is 32 bits:
#   make run CC=emcc EXEC_WRAPPER=node TARGET_EXT=.js

TEST_NAME     := codec
TEST_SRCS     := test.c stuffit.c
TEST_HARNESS  := none
# stuffit.c #includes sit13.c and sit15.c (see there).
EXTRA_SRCS    := $(filter-out %/sit13.c %/sit15.c,$(wildcard ../../../../src/codec/lib/*.c))
INCLUDE_FLAGS  = -I$(UNIT_ROOT)/support $(CODEC_INCLUDES)
# Native builds run under ASan + UBSan, and UB aborts rather than printing
# and carrying on: an overrun of a few bytes lands in allocator padding and
# would pass unseen without a sanitizer.  Not under emcc: the wasm32 run is
# for 32-bit arithmetic.
SANITIZE     ?= $(if $(filter emcc%,$(notdir $(firstword $(CC)))),,-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
DETECT_LEAKS  ?= 1
RUN_ENV       := ASAN_OPTIONS=detect_leaks=$(DETECT_LEAKS)
include ../../common.mk
