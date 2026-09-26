# The peeler archive library, tested as an untrusted parser.  See test.c.
#
# No emulator harness and no stubs, and only peeler's own headers on the
# include path: peeler is a self-contained library -- nothing in
# src/peeler/lib includes anything from src/core -- and linking a harness or
# compiling against the core headers would only hide an accidental dependency
# if one were ever added.
#
# test.c #includes formats/sit15.c so that its arithmetic *encoder* can drive
# the decoder's own model code (model_setup/model_bump), and formats/sit13.c
# so that its bit writer uses the decoder's own meta-code tables -- encoder and
# decoder agree by construction.  Both are therefore compiled as part of
# test.c and left out of EXTRA_SRCS below, or their external symbols would be
# defined twice.
#
# The same suite runs on wasm32, where size_t is 32 bits:
#   make run CC=emcc EXEC_WRAPPER=node TARGET_EXT=.js

TEST_NAME     := peeler
TEST_SRCS     := test.c
TEST_HARNESS  := none
EXTRA_SRCS    := ../../../../src/peeler/lib/peeler.c \
                 ../../../../src/peeler/lib/appledouble.c \
                 ../../../../src/peeler/lib/err.c \
                 ../../../../src/peeler/lib/util.c \
                 ../../../../src/peeler/lib/formats/bin.c \
                 ../../../../src/peeler/lib/formats/cpt.c \
                 ../../../../src/peeler/lib/formats/hqx.c \
                 ../../../../src/peeler/lib/formats/sit.c \
                 ../../../../src/peeler/lib/formats/sit3.c
INCLUDE_FLAGS  = -I$(UNIT_ROOT)/support $(PEELER_INCLUDES)
# Native builds run under ASan + UBSan, and UB aborts rather than printing and
# carrying on.  Several peeler defects are small heap overruns (two bytes past
# a malloc) that land in allocator padding and do not crash natively; a
# test of one must fail on the unfixed code, and without a sanitizer it would
# pass for the wrong reason.  Not under emcc: the wasm32 run is for 32-bit
# arithmetic, and ASan there costs more than it finds.
SANITIZE     ?= $(if $(filter emcc%,$(notdir $(firstword $(CC)))),,-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
# Leak detection is on: every decoder's allocations are owned by its decode
# context and freed on abort, and LeakSanitizer is how a new abort path that
# leaks gets caught -- several past leaks were exactly that.
DETECT_LEAKS  ?= 1
RUN_ENV       := ASAN_OPTIONS=detect_leaks=$(DETECT_LEAKS)
include ../../common.mk
