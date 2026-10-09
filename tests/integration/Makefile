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
#   make test TESTS="a b"  - Run just those tests (fixture producers too, if listed)
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

# CI runs by tier and never the unfiltered set, so a test whose config.mk
# has no recognised `TEST_TIER := unit|matrix|extended` line (a typo, `?=`,
# trailing blanks, no config.mk at all) would run nowhere while still being
# listed.  Refuse to proceed instead.
tier_of = $(shell sed -n 's/^TEST_TIER := \(unit\|matrix\|extended\)$$/\1/p' $(1)/config.mk 2>/dev/null)
UNTIERED := $(strip $(foreach d,$(ALL_TEST_DIRS),$(if $(call tier_of,$(d)),,$(d))))
ifneq ($(UNTIERED),)
$(error tests with no recognised TEST_TIER (unit | matrix | extended): $(UNTIERED))
endif

# TIER may name several tiers ("matrix extended"): CI runs them as ONE -j
# pool, so the machine never idles at the boundary between two tails.
space := $(empty) $(empty)
TIER ?=
# TESTS="a b" runs just those directories through the aggregate `test`
# target (fixtures, ordering and the summary included).
TESTS ?=
ifneq ($(strip $(TESTS)),)
UNORDERED_DIRS := $(filter $(ALL_TEST_DIRS),$(TESTS))
else ifeq ($(TIER),)
UNORDERED_DIRS := $(ALL_TEST_DIRS)
else
UNORDERED_DIRS := $(patsubst %/config.mk,%,$(shell grep -lE '^TEST_TIER := ($(subst $(space),|,$(strip $(TIER))))$$' $(addsuffix /config.mk,$(ALL_TEST_DIRS)) 2>/dev/null))
ifeq ($(UNORDERED_DIRS),)
$(error TIER=$(TIER) matches no tests (expected unit, matrix, or extended))
endif
endif

# Longest first (scripts/order-tests.py, weights in test-weights.json):
# make -j starts targets in list order, so the long suites start at once
# instead of when the alphabet reaches them.  SHARD=K/N keeps one of N
# bin-packed shards (CI splits the integration run across runners).
SHARD ?=
TEST_WEIGHTS := test-weights.json
TEST_DIRS := $(shell python3 $(ROOT_DIR)/scripts/order-tests.py $(TEST_WEIGHTS) $(if $(SHARD),--shard $(SHARD)) $(UNORDERED_DIRS))
TEST_TARGETS := $(addprefix test-,$(TEST_DIRS))

# Checkpoint fixtures (scripts/order-tests.py; docs/guide/TESTING.md,
# "Checkpoint fixtures").  Only the aggregate run uses them: `test` clears
# FIXTURE_DIR and runs its targets with FIXTURE_RUN=1, which makes each
# consumer wait for its producer (an order-only prerequisite), hands every
# test --var FIXTURES=, and lets a failed test not stop the targets after it
# (the summary reads the status files, so nothing is lost).  A single
# `make test-<name>` gets no FIXTURES and boots everything itself.
FIXTURE_DIR := $(abspath $(WORK_DIR_BASE))/fixtures
FIXTURE_RUN ?=
ifneq ($(FIXTURE_RUN),)
FIXTURE_EDGES := $(shell python3 $(ROOT_DIR)/scripts/order-tests.py $(TEST_WEIGHTS) --deps $(TEST_DIRS))
$(foreach e,$(FIXTURE_EDGES),$(eval test-$(firstword $(subst :, ,$(e))): | test-$(lastword $(subst :, ,$(e)))))
endif
VALGRIND_TARGETS := $(addprefix test-valgrind-,$(TEST_DIRS))

# Extra --var definitions forwarded to the shell (suite row filters,
# golden regeneration, keep-going nightly mode).
TEST_VARS ?=
export TEST_VARS

# Environment for the runner script (absolute; the runner cds per test).
export HEADLESS_BIN_ABS := $(abspath $(HEADLESS_BIN))
export DUMP_BIN_ABS := $(abspath $(DUMP_BIN))
export TEST_DATA_ABS := $(abspath $(TEST_DATA))

.PHONY: all build test test-valgrind list check-tiers clean check-data FORCE
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

# The dump tool — used by the re-* tests for resource-fork extraction.  Every
# test rule depends on it (phony, so a stale binary is rebuilt too).
$(DUMP_BIN):
	@$(MAKE) -C $(ROOT_DIR)/tools/dump

# Run the (tier-filtered) tests.  The recursive make inherits -j, so
# `make test -j8` runs tests concurrently; -k keeps going past failures
# and the summary below reads the per-test status files the runner
# writes, which stays accurate under parallel interleaving.
test: check-data $(HEADLESS_BIN) $(DUMP_BIN)
	@echo "========================================"
	@echo "Running $(words $(TEST_DIRS)) integration test(s)$(if $(TIER), [tier: $(TIER)])$(if $(SHARD), [shard $(SHARD)])"
	@$(if $(DATA_LINE),echo "$(DATA_LINE)",true)
	@echo "========================================"
	@mkdir -p $(TEST_RESULTS_BASE) && : > $(TEST_RESULTS_BASE)/durations.jsonl
	@rm -rf $(FIXTURE_DIR) && mkdir -p $(FIXTURE_DIR)
	@$(MAKE) --no-print-directory -k $(TEST_TARGETS) FIXTURE_RUN=1; \
	failed=""; skipped=""; \
	for dir in $(TEST_DIRS); do \
		[ "$$(cat "$(TEST_RESULTS_BASE)/$$dir/status" 2>/dev/null)" = "PASS" ] || failed="$$failed $$dir"; \
		[ ! -f "$(TEST_RESULTS_BASE)/$$dir/skipped" ] || skipped="$$skipped $$dir"; \
	done; \
	echo "========================================"; \
	[ -z "$$skipped" ] || echo "SKIPPED (no media):$$skipped"; \
	if [ -z "$$failed" ]; then \
		echo "All $(words $(TEST_DIRS)) integration test(s) passed$${skipped:+, $$(echo $$skipped | wc -w) of them skipped}"; \
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
			name=$$(sed -n 's/^TEST_NAME[[:space:]]*:=[[:space:]]*//p' "$$dir/config.mk" | head -1); \
			tier=$$(sed -n 's/^TEST_TIER := \(unit\|matrix\|extended\)$$/\1/p' "$$dir/config.mk"); \
			echo "  $$dir [$$tier]: $$name"; \
		else \
			echo "  $$dir"; \
		fi; \
	done

# Every test directory has a recognised tier (checked at parse time above);
# this target exists so CI can run that check on its own, without data.
check-tiers:
	@echo "$(words $(ALL_TEST_DIRS)) integration tests, each with a tier"

# Pattern rule for running individual tests: delegate to the shared
# runner with per-test result/work dirs.
# WRAPPER is a command prefix for the headless binary (valgrind below; a
# script that appends a flag, scripts/compare-jobs-inline.sh); empty by
# default.
WRAPPER ?=
test-%: $(HEADLESS_BIN) $(DUMP_BIN) FORCE
	$(if $(FIXTURE_RUN),-)@HEADLESS_BIN="$(HEADLESS_BIN_ABS)" \
	DUMP_BIN="$(DUMP_BIN_ABS)" \
	TEST_DATA="$(TEST_DATA_ABS)" \
	TEST_RESULTS_DIR="$(abspath $(TEST_RESULTS_BASE))/$*" \
	WORK_DIR="$(abspath $(WORK_DIR_BASE))/$*" \
	FIXTURE_DIR="$(if $(FIXTURE_RUN),$(FIXTURE_DIR))" \
	WRAPPER="$(WRAPPER)" \
	bash $(RUNNER) "$*"

# Clean integration test artifacts (headless binary is cleaned by top-level make)
clean:
	rm -rf $(TEST_RESULTS_BASE) $(WORK_DIR_BASE)

# Run the (tier-filtered) tests under Valgrind memcheck.  Note: the
# full-suite sweep is impractical (20-50x slowdown over billions of
# guest cycles); CI runs `make test-valgrind TIER=unit` plus selected
# boots — see docs/guide/TESTING.md.
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
test-valgrind-%: $(HEADLESS_BIN) $(DUMP_BIN) FORCE
	@HEADLESS_BIN="$(HEADLESS_BIN_ABS)" \
	DUMP_BIN="$(DUMP_BIN_ABS)" \
	TEST_DATA="$(TEST_DATA_ABS)" \
	TEST_RESULTS_DIR="$(abspath $(TEST_RESULTS_BASE))/$*" \
	WORK_DIR="$(abspath $(WORK_DIR_BASE))/$*" \
	WRAPPER="$(VALGRIND) $(VALGRIND_FLAGS)" \
	bash $(RUNNER) "$*"
