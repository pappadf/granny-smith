# PPC (MPC601) core unit test (proposal-powerpc-601-pdm.md Phase B §7.2):
# directed semantics tests from the 601UM chapter-10 RTL — carry/overflow/
# CR0 record matrices, shift/rotate mask edges, the POWER holdovers against
# their documented RTL, MQ side effects, SPR asymmetries, the exception
# model, and the alignment rules.  Uses the cpu harness for real memory
# (the main-CPU core reads through the global fast path); the test builds
# its own 32-bit context instead of the harness's 24-bit Plus layout.

TEST_NAME := ppc
TEST_SRCS := test.c
TEST_HARNESS := cpu
EXTRA_SRCS := ../../../../src/core/cpu/ppc/ppc.c \
              ../../../../src/core/cpu/ppc/ppc_run.c \
              ../../../../src/core/cpu/ppc/ppc_mmu.c \
              ../../../../src/core/cpu/ppc/ppc_fpu.c \
              ../../../../src/core/cpu/ppc/ppc_disasm.c
EXTRA_CFLAGS := -I$(abspath ../../../../src/core/cpu/ppc)
include ../../common.mk
