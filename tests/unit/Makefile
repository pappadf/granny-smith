## Orchestrator Makefile for unit test subdirectories
# Each test provides its own Makefile including common.mk.
# This file discovers test directories under suites/ and delegates.

# Discover test directories under suites/ (each must contain a Makefile)
TEST_DIRS := $(sort $(dir $(wildcard suites/*/Makefile)))
TESTS := $(patsubst %/,%,$(TEST_DIRS))
BUILD_DIR := build

.PHONY: all run run-wasm32 list clean $(TESTS) test-%

all: $(TESTS)
	@echo "Built $(words $(TESTS)) test(s)"

# Delegate build to each sub-make
$(TESTS):
	@echo "[MAKE] $@"; $(MAKE) -C $@ all

run: $(TESTS)
	@failed=""; passed=0; total=0; \
	for d in $(TESTS); do \
	  total=$$((total + 1)); \
	  echo "[RUN ] $$d"; \
	  if $(MAKE) -C $$d run; then \
	    passed=$$((passed + 1)); \
	  else \
	    echo "[FAIL] $$d"; \
	    failed="$$failed $$d"; \
	  fi; \
	done; \
	echo ""; \
	if [ -z "$$failed" ]; then \
	  echo "All $$total tests passed"; \
	else \
	  echo "$$passed/$$total passed, FAILED:$$failed"; \
	  exit 1; \
	fi

# Run a single test suite: make test-<name>
test-%:
	$(MAKE) -C suites/$* run

# The parser suites again, on wasm32 (09-WORK-ORDER.md §0.2, A0.2).
#
# The shipping build is emcc -> WebAssembly, where size_t and long are 32 bits;
# the native tier is 64-bit and cannot see a defect that only wraps at 32 (an
# overflowing `(size_t)a + b`, a `(long)` seek offset).  So the suites that
# parse untrusted input or do storage arithmetic are rebuilt with emcc and run
# under node.  Separate BUILD_DIR, so the native binaries are never touched.
# The first line of output from this target is the probe's word size: if it
# says 64, the run is not testing what it claims to.
WASM32_SUITES := peeler resfork ndif udif hfsplus appledouble storage vfs checkpoint
WASM32_BUILD  := $(abspath $(BUILD_DIR))-wasm32
# The shipping build's stack (Makefile: -s STACK_SIZE=5MB), not emscripten's
# 64 KB default -- a recursion bound that is right for the product would
# otherwise trap here, and one tuned to pass here would be wrong for it.
WASM32_LDFLAGS := -sSTACK_SIZE=5MB

run-wasm32:
	@command -v emcc >/dev/null || { echo "run-wasm32: emcc not found"; exit 1; }
	@mkdir -p $(WASM32_BUILD)
	@printf '#include <stdio.h>\nint main(void){printf("[wasm32] size_t is %%u bits, long is %%u bits\\n",(unsigned)(8*sizeof(size_t)),(unsigned)(8*sizeof(long)));return sizeof(size_t)==4?0:1;}\n' > $(WASM32_BUILD)/probe.c
	@emcc $(WASM32_BUILD)/probe.c -o $(WASM32_BUILD)/probe.js && node $(WASM32_BUILD)/probe.js
	@failed=""; \
	for s in $(WASM32_SUITES); do \
	  echo "[RUN ] wasm32 $$s"; \
	  if $(MAKE) -C suites/$$s run CC=emcc EXEC_WRAPPER=node BUILD_DIR=$(WASM32_BUILD) TARGET_EXT=.js \
	       LDFLAGS="$(WASM32_LDFLAGS)"; then :; \
	  else echo "[FAIL] wasm32 $$s"; failed="$$failed $$s"; fi; \
	done; \
	if [ -z "$$failed" ]; then echo "All $(words $(WASM32_SUITES)) wasm32 suites passed"; \
	else echo "wasm32 FAILED:$$failed"; exit 1; fi

list:
	@echo $(TESTS)

clean:
	@for d in $(TESTS); do $(MAKE) -C $$d clean; done
	rm -rf $(BUILD_DIR) $(WASM32_BUILD)
