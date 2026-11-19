# The peeler archive library, tested as an untrusted parser.  See test.c.
#
# No emulator harness and no stubs, and only peeler's own headers and the
# codec library's on the include path: peeler depends on nothing but the
# codec library (src/codec) -- nothing in src/peeler/lib includes anything
# from src/core -- and linking a harness or compiling against the core
# headers would only hide an accidental dependency if one were ever added.
#
# The method decoders themselves (StuffIt's, Compact Pro's, DEFLATE's) are
# the codec library's, and the codec suite tests them; this suite tests them
# through the containers, and DEFLATE with the writer in zipgz.c.
#
# The same suite runs on wasm32, where size_t is 32 bits:
#   make run CC=emcc EXEC_WRAPPER=node TARGET_EXT=.js

TEST_NAME     := peeler
TEST_SRCS     := test.c zipgz.c
TEST_HARNESS  := none
EXTRA_SRCS    := $(sort $(wildcard ../../../../src/peeler/lib/*.c ../../../../src/peeler/lib/formats/*.c)) \
                 $(wildcard ../../../../src/codec/lib/*.c)
INCLUDE_FLAGS  = -I$(UNIT_ROOT)/support $(PEELER_INCLUDES)
# Native builds run under ASan + UBSan, and UB aborts rather than printing and
# carrying on.  Several peeler defects are small heap overruns (two bytes past
# a malloc) that land in allocator padding and do not crash natively; a
# test of one must fail on the unfixed code, and without a sanitizer it would
# pass for the wrong reason.  Not under emcc: the wasm32 run is for 32-bit
# arithmetic, and ASan there costs more than it finds.
SANITIZE     ?= $(if $(filter emcc%,$(notdir $(firstword $(CC)))),,-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
# Leak detection is on: every container decoder's allocations are owned by
# its decode context and freed on abort, and LeakSanitizer is how a new abort path that
# leaks gets caught -- several past leaks were exactly that.
DETECT_LEAKS  ?= 1
RUN_ENV       := ASAN_OPTIONS=detect_leaks=$(DETECT_LEAKS)
include ../../common.mk
