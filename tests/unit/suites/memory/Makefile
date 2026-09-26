# Memory unit test
# Tests parameterised memory_map_init() with 24-bit and 32-bit address spaces.
# Uses real memory.c instead of stub_memory.c to test the actual implementation.

TEST_NAME    := memory
TEST_SRCS    := test.c
TEST_HARNESS := isolated
# Real memory.c is included; stub_memory.c is excluded to avoid symbol conflicts
OMIT_STUBS   := memory
STUBS        := cpu lisa_mmu
EXTRA_SRCS   := ../../../../src/core/memory/memory.c \
                ../../../../src/core/memory/mmu.c \
                ../../../../src/core/memory/mmu040.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c
include ../../common.mk
