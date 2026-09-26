# MMU unit test (Milestone 7)
# Tests 68030 PMMU: SoA arrays, table walk, TLB invalidation, TT, PTEST.
# Uses real memory.c and mmu.c.

TEST_NAME    := mmu
TEST_SRCS    := test.c
TEST_HARNESS := isolated
# Real memory.c and mmu.c; exclude stub_memory.c to avoid symbol conflicts
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
