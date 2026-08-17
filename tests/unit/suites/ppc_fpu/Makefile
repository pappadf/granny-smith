# PPC (MPC601) FPU datapath unit test (proposal-powerpc-601-pdm.md §3.6,
# Phase E): directed 601UM §5.4.7 semantics, a randomized host-double
# oracle, interpreter-level CR1/trap integration, and the cross-host
# corpus digest.  `make wasm-check` builds the same corpus sweep with
# emcc, runs it under node, and diffs the digests — the native-vs-WASM
# byte-exactness acceptance (skipped with a note when emcc is absent).

TEST_NAME := ppc_fpu
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

# --- native-vs-WASM byte exactness ------------------------------------------
# The corpus runner is dependency-free (kernel + fpu_corpus.h only), so the
# WASM build needs nothing but emcc and node.

WASM_DIR := $(BUILD_DIR)/obj/$(TEST_NAME)-wasm
KERNEL := $(abspath ../../../../src/core/cpu/ppc/ppc_softfp.c)

.PHONY: wasm-check
wasm-check: run
	@command -v emcc >/dev/null 2>&1 || { echo "wasm-check: emcc not found — skipped"; exit 0; }; \
	mkdir -p $(WASM_DIR); \
	cc -O2 -I$(abspath ../../../../src/core/cpu/ppc) $(abspath wasm_hash.c) $(KERNEL) -o $(WASM_DIR)/hash-native && \
	emcc -O2 -I$(abspath ../../../../src/core/cpu/ppc) $(abspath wasm_hash.c) $(KERNEL) -o $(WASM_DIR)/hash-wasm.js && \
	$(WASM_DIR)/hash-native > $(WASM_DIR)/native.txt && \
	node $(WASM_DIR)/hash-wasm.js > $(WASM_DIR)/wasm.txt && \
	diff $(WASM_DIR)/native.txt $(WASM_DIR)/wasm.txt && \
	echo "wasm-check: corpus digests identical ($$(cat $(WASM_DIR)/native.txt))"
