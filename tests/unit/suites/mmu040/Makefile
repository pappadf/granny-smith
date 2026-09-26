# MMU040 unit test
# Tests the MC68040 MMU: TTRs, three-level table walk, U/M protocol,
# protection, indirect descriptors, 8K pages, PTEST MMUSR.
# Uses real memory.c, mmu.c, and mmu040.c.

TEST_NAME    := mmu040
TEST_SRCS    := test.c
TEST_HARNESS := isolated
# Real memory.c, mmu.c, mmu040.c; exclude stub_memory.c to avoid symbol conflicts
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
