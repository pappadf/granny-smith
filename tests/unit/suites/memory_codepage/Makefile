# Code-page coherence unit test (the predecoded cores' memory-layer half)
# Marks a page as code, asserts its write SoA entries vanish and stay vanished
# across every refill site, and that host-side writers reach the hook.
# Uses real memory.c instead of stub_memory.c to test the actual implementation.

TEST_NAME    := memory_codepage
TEST_SRCS    := test.c
TEST_HARNESS := isolated
# Real memory.c is included; stub_memory.c is excluded to avoid symbol conflicts
OMIT_STUBS   := memory
STUBS        := cpu lisa_mmu
EXTRA_SRCS   := ../../../../src/core/job/seam.c \
                ../../../../src/core/memory/memory.c \
                ../../../../src/core/memory/mmu.c \
                ../../../../src/core/memory/mmu040.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value.c
include ../../common.mk
