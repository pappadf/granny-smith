# PPC (MPC601) disassembler unit test.
# Vectors are cross-validated against powerpc-linux-gnu-objdump -m powerpc:601
# over both a directed sweep and the shipping PDM ROM's HWInit/nanokernel
# regions (modulo the documented simplified-mnemonic differences).
# Dependency-free C99 — no stubs, and only the disassembler's own header
# directory: no support shims, no forced includes.

TEST_NAME     := ppc_disasm
TEST_SRCS     := test.c
TEST_HARNESS  := none
EXTRA_SRCS    := ../../../../src/core/cpu/ppc/ppc_disasm.c
INCLUDE_FLAGS  = -I$(EMU_ROOT)/core/cpu/ppc
include ../../common.mk
