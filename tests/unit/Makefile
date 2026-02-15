## Orchestrator Makefile for unit test subdirectories
# Each test provides its own Makefile including common.mk.
# This file discovers test directories under suites/ and delegates.

# Discover test directories under suites/ (each must contain a Makefile)
TEST_DIRS := $(sort $(dir $(wildcard suites/*/Makefile)))
TESTS := $(patsubst %/,%,$(TEST_DIRS))
BUILD_DIR := build

.PHONY: all run list clean $(TESTS) test-%

all: $(TESTS)
	@echo "Built $(words $(TESTS)) test(s)"

# Delegate build to each sub-make
$(TESTS):
	@echo "[MAKE] $@"; $(MAKE) -C $@ all

run: $(TESTS)
	@set -e; \
	for d in $(TESTS); do \
	  echo "[RUN ] $$d"; \
	  $(MAKE) -C $$d run || { echo "[FAIL] $$d"; exit 1; }; \
	done; \
	echo "All $(words $(TESTS)) tests passed";

# Run a single test suite: make test-<name>
test-%:
	$(MAKE) -C suites/$* run

list:
	@echo $(TESTS)

clean:
	@for d in $(TESTS); do $(MAKE) -C $$d clean; done
	rm -rf $(BUILD_DIR)
