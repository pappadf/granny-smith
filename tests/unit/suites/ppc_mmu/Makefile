# PPC (MPC601) MMU front-end unit test (proposal-powerpc-601-pdm.md
# Phase D §3.5): 601-format BATs with key/PP protection, T=1 memory-forced
# segments (incl. the SR5-toggle alias and the DT-off data path), the
# hashed page table walk with R/C write-back, DSI/ISI register exactness,
# the (PR,DT)-keyed SoA discipline, tlbie congruence-class invalidation,
# and dcbz's W/I alignment rule.  Same harness shape as the ppc suite.

TEST_NAME := ppc_mmu
TEST_SRCS := test.c
TEST_HARNESS := cpu
EXTRA_SRCS := ../../../../src/core/cpu/ppc/ppc.c \
              ../../../../src/core/cpu/ppc/ppc_run.c \
              ../../../../src/core/cpu/ppc/ppc_mmu.c \
              ../../../../src/core/cpu/ppc/ppc_fpu.c \
              ../../../../src/core/cpu/ppc/ppc_disasm.c
EXTRA_CFLAGS := -I$(abspath ../../../../src/core/cpu/ppc)
include ../../common.mk
