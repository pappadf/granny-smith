# The byte-source layer (src/core/storage/source.h): views and keys,
# decode-through, the chunk cache's coalescing and spill, and the format
# registry's detection budget and wrapper unwrapping.  Runs under ASan +
# UBSan natively; the coalescing test runs eight threads on one chunk.

TEST_NAME := source
TEST_SRCS := test.c
TEST_HARNESS := isolated
EXTRA_SRCS :=
USE_SOURCE_LAYER := 1

SANITIZE := $(if $(filter emcc%,$(notdir $(firstword $(CC)))),,-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
EXTRA_CFLAGS := $(SANITIZE)
LDFLAGS += $(SANITIZE) -lpthread

include ../../common.mk
