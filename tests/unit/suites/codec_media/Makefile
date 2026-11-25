# The codec library's media codecs (src/codec): streaming zlib, ZMBV and
# FLAC, encoder to decoder.  See test.c.
#
# Like the codec suite: no emulator harness, no stubs, only the codec's own
# headers.  Natively the streaming zlib output is also inflated by the
# system zlib (HAVE_ZLIB), an independent decoder; the wasm32 run has no
# zlib and checks against the in-tree inflater alone:
#   make run CC=emcc EXEC_WRAPPER=node TARGET_EXT=.js

TEST_NAME     := codec_media
TEST_SRCS     := test.c
TEST_HARNESS  := none
EXTRA_SRCS    := $(wildcard ../../../../src/codec/lib/*.c)
INCLUDE_FLAGS  = -I$(UNIT_ROOT)/support $(CODEC_INCLUDES)
IS_EMCC       := $(filter emcc%,$(notdir $(firstword $(CC))))
EXTRA_CFLAGS  := $(if $(IS_EMCC),,-DHAVE_ZLIB)
EXTRA_LDFLAGS := $(if $(IS_EMCC),,-lz)
SANITIZE     ?= $(if $(IS_EMCC),,-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
RUN_ENV       := ASAN_OPTIONS=detect_leaks=1
include ../../common.mk
