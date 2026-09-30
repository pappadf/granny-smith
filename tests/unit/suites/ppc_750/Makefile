# PPC 750 model unit test: the MPC750 implementation registers the 604
# model lacks — L2CR and its global-invalidate handshake, read-only HID1
# (PLL_CFG), THRM1-3 TIV/TIN, ICTC, the 750 performance-monitor map, the
# absent PIR/SDA, self-clearing HID0[ICFI/DCFI], tlbia and fsqrt illegal,
# and the reset state (MPC750UM Tables 2-19, 2-48, 2-49; §9.1.5; §10.3).

TEST_NAME := ppc_750
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
