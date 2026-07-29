# Integration Tests Runner
#
# Auto-discovers and runs all integration tests under tests/integration/
# Each test subdirectory must contain:
#   - test.script: Shell commands to run the test
#   - config.mk: Test configuration (ROM, disk images, arguments, tier)
#
# The per-test runner logic lives in scripts/run-integration-test.sh
# (one copy for the plain and valgrind flavors); this Makefile handles
# discovery, tier filtering, parallelism, and the summary.
#
# Usage:
#   make test              - Run all integration tests
#   make test TIER=matrix  - Run one tier (unit | matrix | extended)
#   make test -j$(nproc)   - Run tests in parallel (safe: per-test
#                            GS_STORAGE_CACHE keeps tests/data pristine)
#   make test-<name>       - Run a specific test (e.g. make test-boot-matrix)
#   make test-<name> TEST_VARS="ROW=x REGEN=1"  - extra --var definitions
#   make list              - List all available tests (with tiers)
#   make build             - Build the headless emulator only
#   make check-data        - Check if test data is available

# Paths
ROOT_DIR := ../..
HEADLESS_BIN := $(ROOT_DIR)/build/headless/gs-headless
DUMP_BIN := $(ROOT_DIR)/tools/dump/dump
TEST_DATA := $(ROOT_DIR)/tests/data
TEST_RESULTS_BASE := $(ROOT_DIR)/tests/integration/test-results
WORK_DIR_BASE := $(ROOT_DIR)/build/integration
FETCH_SCRIPT := $(ROOT_DIR)/scripts/fetch-test-data.sh
RUNNER := $(ROOT_DIR)/scripts/run-integration-test.sh

# Which gs-test-data revision the goldens were captured against, and which one
# is actually on disk. Printed in the run header so a stale-media failure is
# diagnosable from the log alone rather than looking like a pile of golden
# regressions. Both are empty on a checkout with no pin or pre-pin data, and
# the header then simply says nothing.
DATA_PIN := $(shell grep -vE '^[[:space:]]*(\#|$$)' $(ROOT_DIR)/tests/data-version 2>/dev/null | head -1 | tr -d '[:space:]')
DATA_HAVE := $(shell sed -n 's/^\# Commit: //p' $(TEST_DATA)/.gs-test-data-marker 2>/dev/null | head -1 | tr -d '[:space:]')
ifeq ($(DATA_HAVE),)
DATA_LINE :=
else ifeq ($(DATA_PIN),$(DATA_HAVE))
DATA_LINE := test data: $(shell echo $(DATA_HAVE) | cut -c1-12) (pinned)
else ifeq ($(DATA_PIN),)
DATA_LINE := test data: $(shell echo $(DATA_HAVE) | cut -c1-12) (no pin recorded)
else
DATA_LINE := !! test data: $(shell echo $(DATA_HAVE) | cut -c1-12) but tests/data-version pins $(shell echo $(DATA_PIN) | cut -c1-12) -- goldens may not match this media
endif

# Valgrind configuration for memcheck
# --error-exitcode=1: Return non-zero on memory errors
# --leak-check=full: Detailed leak reporting
# --track-origins=yes: Track origin of uninitialized values
# --show-leak-kinds=definite,possible: Report definite and possible leaks
# --errors-for-leak-kinds=definite: Only fail on definite leaks
VALGRIND := valgrind
VALGRIND_FLAGS := --error-exitcode=1 \
                  --leak-check=full \
                  --track-origins=yes \
                  --show-leak-kinds=definite,possible \
                  --errors-for-leak-kinds=definite \
                  --quiet

# Auto-discover test directories (those containing test.script); TIER=
# narrows to directories whose config.mk declares that TEST_TIER.
ALL_TEST_DIRS := $(patsubst %/test.script,%,$(wildcard */test.script))
TIER ?=
ifeq ($(TIER),)
TEST_DIRS := $(ALL_TEST_DIRS)
else
TEST_DIRS := $(patsubst %/config.mk,%,$(shell grep -l '^TEST_TIER := $(TIER)$$' $(addsuffix /config.mk,$(ALL_TEST_DIRS)) 2>/dev/null))
ifeq ($(TEST_DIRS),)
$(error TIER=$(TIER) matches no tests (expected unit, matrix, or extended))
endif
endif
TEST_TARGETS := $(addprefix test-,$(TEST_DIRS))
VALGRIND_TARGETS := $(addprefix test-valgrind-,$(TEST_DIRS))

# Extra --var definitions forwarded to the shell (suite row filters,
# golden regeneration, keep-going nightly mode).
TEST_VARS ?=
export TEST_VARS

# Environment for the runner script (absolute; the runner cds per test).
export HEADLESS_BIN_ABS := $(abspath $(HEADLESS_BIN))
export DUMP_BIN_ABS := $(abspath $(DUMP_BIN))
export TEST_DATA_ABS := $(abspath $(TEST_DATA))

.PHONY: all build test test-valgrind list clean check-data FORCE
FORCE:

# Check if test data is available
check-data:
	@$(FETCH_SCRIPT) --check >/dev/null 2>&1; rc=$$?; \
	if [ $$rc -eq 0 ]; then \
		echo "Test data is available"; \
	elif [ $$rc -eq 2 ]; then \
		echo ""; \
		echo "========================================"; \
		echo "ERROR: Test data is the WRONG REVISION"; \
		echo "========================================"; \
		$(FETCH_SCRIPT) --check 2>&1 | tail -n +2; \
		exit 1; \
	else \
		echo ""; \
		echo "========================================"; \
		echo "ERROR: Test data is not available"; \
		echo "========================================"; \
		echo ""; \
		echo "Integration tests require proprietary test data (ROM images,"; \
		echo "disk images) that cannot be distributed with this open source"; \
		echo "project."; \
		echo ""; \
		echo "For maintainers:"; \
		echo "  Run: ./scripts/fetch-test-data.sh"; \
		echo "  (requires GS_TEST_DATA_TOKEN environment variable)"; \
		echo ""; \
		echo "For contributors:"; \
		echo "  See docs/guide/TEST_DATA.md for information on obtaining or"; \
		echo "  providing your own test data."; \
		echo ""; \
		echo "========================================"; \
		exit 1; \
	fi

# Default: build and run all tests
all: build test

# Build the headless emulator
# Note: Always invoke the build to ensure proper dependency tracking
# The headless Makefile will determine if rebuild is needed
build:
	@$(MAKE) -C $(ROOT_DIR) -f Makefile.headless

# Ensure headless binary exists and is up to date
# Mark as PHONY so Make always checks if rebuild is needed
.PHONY: $(HEADLESS_BIN) $(DUMP_BIN)
$(HEADLESS_BIN):
	@$(MAKE) -C $(ROOT_DIR) -f Makefile.headless

# The dump tool — used by the re-* tests for resource-fork extraction.
# Each test runner can depend on it via $(DUMP_BIN) in its config.mk.
$(DUMP_BIN):
	@$(MAKE) -C $(ROOT_DIR)/tools/dump

# Run the (tier-filtered) tests.  The recursive make inherits -j, so
# `make test -j8` runs tests concurrently; -k keeps going past failures
# and the summary below reads the per-test status files the runner
# writes, which stays accurate under parallel interleaving.
test: check-data $(HEADLESS_BIN) $(DUMP_BIN)
	@echo "========================================"
	@echo "Running $(words $(TEST_DIRS)) integration test(s)$(if $(TIER), [tier: $(TIER)])"
	@$(if $(DATA_LINE),echo "$(DATA_LINE)",true)
	@echo "========================================"
	@$(MAKE) --no-print-directory -k $(TEST_TARGETS); \
	failed=""; \
	for dir in $(TEST_DIRS); do \
		[ "$$(cat "$(TEST_RESULTS_BASE)/$$dir/status" 2>/dev/null)" = "PASS" ] || failed="$$failed $$dir"; \
	done; \
	echo "========================================"; \
	if [ -z "$$failed" ]; then \
		echo "All $(words $(TEST_DIRS)) integration test(s) passed"; \
	else \
		echo "FAILED:$$failed"; \
		echo "$$(echo $$failed | wc -w) of $(words $(TEST_DIRS)) test(s) FAILED"; \
		exit 1; \
	fi

# List available tests
list:
	@echo "Available integration tests:"
	@for dir in $(TEST_DIRS); do \
		if [ -f "$$dir/config.mk" ]; then \
			name=$$(grep -m1 '^TEST_NAME' "$$dir/config.mk" 2>/dev/null | cut -d= -f2- | sed 's/^[ \t]*//'); \
			tier=$$(grep -m1 '^TEST_TIER' "$$dir/config.mk" 2>/dev/null | cut -d= -f2- | sed 's/^[ \t]*//'); \
			echo "  $$dir [$$tier]: $$name"; \
		else \
			echo "  $$dir"; \
		fi; \
	done

# Pattern rule for running individual tests: delegate to the shared
# runner with per-test result/work dirs.
test-%: $(HEADLESS_BIN) FORCE
	@HEADLESS_BIN="$(HEADLESS_BIN_ABS)" \
	DUMP_BIN="$(DUMP_BIN_ABS)" \
	TEST_DATA="$(TEST_DATA_ABS)" \
	TEST_RESULTS_DIR="$(abspath $(TEST_RESULTS_BASE))/$*" \
	WORK_DIR="$(abspath $(WORK_DIR_BASE))/$*" \
	WRAPPER="" \
	bash $(RUNNER) "$*"

# Clean integration test artifacts (headless binary is cleaned by top-level make)
clean:
	rm -rf $(TEST_RESULTS_BASE) $(WORK_DIR_BASE)

# Run the (tier-filtered) tests under Valgrind memcheck.  Note: the
# full-suite sweep is impractical (20-50x slowdown over billions of
# guest cycles); CI runs `make test-valgrind TIER=unit` plus selected
# boots — see proposal-integration-test-rework §5.4.
test-valgrind: check-data $(HEADLESS_BIN) $(DUMP_BIN)
	@echo "========================================"
	@echo "Running $(words $(TEST_DIRS)) integration test(s) under Valgrind$(if $(TIER), [tier: $(TIER)])"
	@$(if $(DATA_LINE),echo "$(DATA_LINE)",true)
	@echo "========================================"
	@$(MAKE) --no-print-directory -k $(VALGRIND_TARGETS); \
	failed=""; \
	for dir in $(TEST_DIRS); do \
		[ "$$(cat "$(TEST_RESULTS_BASE)/$$dir/status" 2>/dev/null)" = "PASS" ] || failed="$$failed $$dir"; \
	done; \
	echo "========================================"; \
	if [ -z "$$failed" ]; then \
		echo "All $(words $(TEST_DIRS)) integration test(s) passed (valgrind)"; \
	else \
		echo "FAILED:$$failed"; \
		echo "$$(echo $$failed | wc -w) of $(words $(TEST_DIRS)) test(s) FAILED (valgrind)"; \
		exit 1; \
	fi

# Valgrind flavor of the pattern rule: same runner, wrapped binary.
test-valgrind-%: $(HEADLESS_BIN) FORCE
	@HEADLESS_BIN="$(HEADLESS_BIN_ABS)" \
	DUMP_BIN="$(DUMP_BIN_ABS)" \
	TEST_DATA="$(TEST_DATA_ABS)" \
	TEST_RESULTS_DIR="$(abspath $(TEST_RESULTS_BASE))/$*" \
	WORK_DIR="$(abspath $(WORK_DIR_BASE))/$*" \
	WRAPPER="$(VALGRIND) $(VALGRIND_FLAGS)" \
	bash $(RUNNER) "$*"
