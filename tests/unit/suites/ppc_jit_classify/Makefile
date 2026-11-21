# PPC translation-tier classifier unit test (ppc_jit_classify.h): the
# decode tree's fourth instantiation, which the wasm-only translator
# (ppc_jit.c) classifies words with.  The header is host code, so this
# runs it natively: no word the interpreter rejects is ever translated,
# the translator's shapes and fields, the 601 rule, and the FP helper's
# decoder instantiation (ppc_execute_fp).

TEST_NAME := ppc_jit_classify
TEST_SRCS := test.c
TEST_HARNESS := cpu
EXTRA_SRCS := ../../../../src/core/cpu/ppc/ppc.c \
              ../../../../src/core/cpu/ppc/ppc_run.c \
              ../../../../src/core/cpu/ppc/ppc_mmu.c \
              ../../../../src/core/cpu/ppc/ppc_fpu.c \
              ../../../../src/core/cpu/ppc/ppc_softfp.c \
              ../../../../src/core/cpu/ppc/ppc_disasm.c
EXTRA_CFLAGS := -I$(abspath ../../../../src/core/cpu/ppc)
include ../../common.mk
