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

list:
	@echo $(TESTS)

clean:
	@for d in $(TESTS); do $(MAKE) -C $$d clean; done
	rm -rf $(BUILD_DIR)
