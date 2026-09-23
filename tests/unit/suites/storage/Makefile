TEST_NAME := storage
TEST_SRCS := test.c
TEST_HARNESS := isolated
EXTRA_SRCS := ../../../../src/core/storage/storage.c
# On wasm32 the suite's files live on the host filesystem (NODERAWFS), not
# emscripten's in-memory one: the 64-bit offset test writes a block past
# 2 GiB of a sparse delta file, which MEMFS would have to allocate whole.
# `override` because run-wasm32 passes LDFLAGS on the command line.
ifneq ($(filter emcc%,$(notdir $(firstword $(CC)))),)
override LDFLAGS += -sNODERAWFS=1
endif

include ../../common.mk
