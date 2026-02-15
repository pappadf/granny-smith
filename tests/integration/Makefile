# Integration Tests Runner
#
# Auto-discovers and runs all integration tests under tests/integration/
# Each test subdirectory must contain:
#   - test.script: Shell commands to run the test
#   - config.mk: Test configuration (ROM, disk images, arguments)
#
# Usage:
#   make test          - Run all integration tests
#   make test-<name>   - Run a specific test (e.g., make test-boot)
#   make list          - List all available tests
#   make build         - Build the headless emulator only
#   make check-data    - Check if test data is available

# Paths
ROOT_DIR := ../..
HEADLESS_BIN := $(ROOT_DIR)/build/headless/gs-headless
TEST_DATA := $(ROOT_DIR)/tests/data
STORAGE_CACHE := $(ROOT_DIR)/build/storage-cache
TEST_RESULTS_BASE := $(ROOT_DIR)/tests/integration/test-results
FETCH_SCRIPT := $(ROOT_DIR)/scripts/fetch-test-data.sh

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

# Auto-discover test directories (those containing test.script)
TEST_DIRS := $(patsubst %/test.script,%,$(wildcard */test.script))
TEST_TARGETS := $(addprefix test-,$(TEST_DIRS))

.PHONY: all build test test-valgrind list clean check-data FORCE
FORCE:

# Check if test data is available
check-data:
	@if $(FETCH_SCRIPT) --check 2>/dev/null; then \
		echo "Test data is available"; \
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
		echo "  See docs/TEST_DATA.md for information on obtaining or"; \
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
.PHONY: $(HEADLESS_BIN)
$(HEADLESS_BIN):
	@$(MAKE) -C $(ROOT_DIR) -f Makefile.headless

# Run all integration tests (checks for test data first)
test: check-data $(HEADLESS_BIN)
	@echo "========================================"
	@echo "Running $(words $(TEST_DIRS)) integration test(s)"
	@echo "========================================"
	@failed=0; \
	for dir in $(TEST_DIRS); do \
		$(MAKE) --no-print-directory test-$$dir || failed=$$((failed + 1)); \
	done; \
	echo "========================================"; \
	if [ $$failed -eq 0 ]; then \
		echo "All $(words $(TEST_DIRS)) integration test(s) passed"; \
	else \
		echo "$$failed of $(words $(TEST_DIRS)) test(s) FAILED"; \
		exit 1; \
	fi

# List available tests
list:
	@echo "Available integration tests:"
	@for dir in $(TEST_DIRS); do \
		if [ -f "$$dir/config.mk" ]; then \
			name=$$(grep -m1 '^TEST_NAME' "$$dir/config.mk" 2>/dev/null | cut -d= -f2- | sed 's/^[ \t]*//'); \
			desc=$$(grep -m1 '^TEST_DESC' "$$dir/config.mk" 2>/dev/null | cut -d= -f2- | sed 's/^[ \t]*//'); \
			echo "  $$dir: $$name"; \
			[ -n "$$desc" ] && echo "         $$desc"; \
		else \
			echo "  $$dir"; \
		fi; \
	done

# Pattern rule for running individual tests
# Loads config.mk from the test directory to get ROM, disk, and arguments
# Supports TEST_SETUP for pre-test setup commands (e.g., unzipping files)
# Supports TEST_RUNNER for custom test scripts (multi-step tests)
test-%: $(HEADLESS_BIN) FORCE
	@if [ ! -d "$*" ]; then \
		echo "ERROR: Test directory '$*' not found"; \
		exit 1; \
	fi
	@if [ ! -f "$*/test.script" ]; then \
		echo "ERROR: Test script '$*/test.script' not found"; \
		exit 1; \
	fi
	@if [ ! -f "$*/config.mk" ]; then \
		echo "ERROR: Test config '$*/config.mk' not found"; \
		exit 1; \
	fi
	@# Extract configuration from config.mk and run with absolute paths
	@TEST_ROM=$$(grep -m1 '^TEST_ROM' "$*/config.mk" | cut -d= -f2- | sed 's/^[ \t]*//'); \
	TEST_ARGS=$$(grep -m1 '^TEST_ARGS' "$*/config.mk" | cut -d= -f2- | sed 's/^[ \t]*//'); \
	TEST_SETUP=$$(grep -m1 '^TEST_SETUP' "$*/config.mk" | cut -d= -f2- | sed 's/^[ \t]*//'); \
	TEST_RUNNER=$$(grep -m1 '^TEST_RUNNER' "$*/config.mk" | cut -d= -f2- | sed 's/^[ \t]*//'); \
	TEST_NAME=$$(grep -m1 '^TEST_NAME' "$*/config.mk" | cut -d= -f2- | sed 's/^[ \t]*//'); \
	echo "=== Running: $$TEST_NAME ($*) ==="; \
	ABS_TEST_DATA="$(abspath $(TEST_DATA))"; \
	ROM_PATH="$$ABS_TEST_DATA/$$TEST_ROM"; \
	if [ ! -f "$$ROM_PATH" ]; then \
		echo "ERROR: ROM not found: $$ROM_PATH"; \
		exit 1; \
	fi; \
	TEST_TMPDIR=$$(mktemp -d); \
	TEST_RESULTS_DIR="$(abspath $(TEST_RESULTS_BASE))/$*"; \
	mkdir -p "$$TEST_RESULTS_DIR"; \
	trap "rm -rf $$TEST_TMPDIR" EXIT; \
	if [ -n "$$TEST_SETUP" ]; then \
		SETUP_CMD=$$(echo "$$TEST_SETUP" | sed "s|\$$(TEST_DATA)|$$ABS_TEST_DATA|g" | sed "s|\$$(TEST_TMPDIR)|$$TEST_TMPDIR|g" | sed "s|\$$(TEST_RESULTS_DIR)|$$TEST_RESULTS_DIR|g"); \
		echo "Setup: $$SETUP_CMD"; \
		eval "$$SETUP_CMD" || { echo "=== FAIL: $* (setup) ==="; exit 1; }; \
	fi; \
	EXPANDED_ARGS=$$(echo "$$TEST_ARGS" | sed "s|\$$(TEST_DATA)|$$ABS_TEST_DATA|g" | sed "s|\$$(TEST_TMPDIR)|$$TEST_TMPDIR|g"); \
	if [ -n "$$TEST_RUNNER" ]; then \
		cd $* && HEADLESS_BIN=$(abspath $(HEADLESS_BIN)) ROM_PATH="$$ROM_PATH" \
			TEST_DATA="$$ABS_TEST_DATA" TEST_TMPDIR="$$TEST_TMPDIR" \
			TEST_RESULTS_DIR="$$TEST_RESULTS_DIR" \
			STORAGE_CACHE=$(abspath $(STORAGE_CACHE)) \
			bash "$$TEST_RUNNER" || { echo "=== FAIL: $* ==="; exit 1; }; \
	else \
		cd $* && GS_STORAGE_CACHE=$(abspath $(STORAGE_CACHE)) $(abspath $(HEADLESS_BIN)) \
			rom="$$ROM_PATH" \
			$$EXPANDED_ARGS \
			script=test.script \
			--speed=max || { echo "=== FAIL: $* ==="; exit 1; }; \
	fi; \
	echo "=== PASS: $* ==="

# Clean integration test artifacts (headless binary is cleaned by top-level make)
clean:
	rm -rf $(TEST_RESULTS_BASE) $(STORAGE_CACHE)

# Run all integration tests under Valgrind memcheck
test-valgrind: $(HEADLESS_BIN)
	@echo "========================================"
	@echo "Running $(words $(TEST_DIRS)) integration test(s) under Valgrind"
	@echo "========================================"
	@failed=0; \
	for dir in $(TEST_DIRS); do \
		$(MAKE) --no-print-directory test-valgrind-$$dir || failed=$$((failed + 1)); \
	done; \
	echo "========================================"; \
	if [ $$failed -eq 0 ]; then \
		echo "All $(words $(TEST_DIRS)) integration test(s) passed (valgrind)"; \
	else \
		echo "$$failed of $(words $(TEST_DIRS)) test(s) FAILED (valgrind)"; \
		exit 1; \
	fi

# Pattern rule for running individual tests under Valgrind
# Supports TEST_SETUP for pre-test setup commands (e.g., unzipping files)
# Supports TEST_RUNNER for custom test scripts (multi-step tests)
test-valgrind-%: $(HEADLESS_BIN) FORCE
	@if [ ! -d "$*" ]; then \
		echo "ERROR: Test directory '$*' not found"; \
		exit 1; \
	fi
	@if [ ! -f "$*/test.script" ]; then \
		echo "ERROR: Test script '$*/test.script' not found"; \
		exit 1; \
	fi
	@if [ ! -f "$*/config.mk" ]; then \
		echo "ERROR: Test config '$*/config.mk' not found"; \
		exit 1; \
	fi
	@# Extract configuration from config.mk and run with valgrind
	@TEST_ROM=$$(grep -m1 '^TEST_ROM' "$*/config.mk" | cut -d= -f2- | sed 's/^[ \t]*//'); \
	TEST_ARGS=$$(grep -m1 '^TEST_ARGS' "$*/config.mk" | cut -d= -f2- | sed 's/^[ \t]*//'); \
	TEST_SETUP=$$(grep -m1 '^TEST_SETUP' "$*/config.mk" | cut -d= -f2- | sed 's/^[ \t]*//'); \
	TEST_RUNNER=$$(grep -m1 '^TEST_RUNNER' "$*/config.mk" | cut -d= -f2- | sed 's/^[ \t]*//'); \
	TEST_NAME=$$(grep -m1 '^TEST_NAME' "$*/config.mk" | cut -d= -f2- | sed 's/^[ \t]*//'); \
	echo "=== Running (valgrind): $$TEST_NAME ($*) ==="; \
	ABS_TEST_DATA="$(abspath $(TEST_DATA))"; \
	ROM_PATH="$$ABS_TEST_DATA/$$TEST_ROM"; \
	if [ ! -f "$$ROM_PATH" ]; then \
		echo "ERROR: ROM not found: $$ROM_PATH"; \
		exit 1; \
	fi; \
	TEST_TMPDIR=$$(mktemp -d); \
	TEST_RESULTS_DIR="$(abspath $(TEST_RESULTS_BASE))/$*"; \
	mkdir -p "$$TEST_RESULTS_DIR"; \
	trap "rm -rf $$TEST_TMPDIR" EXIT; \
	if [ -n "$$TEST_SETUP" ]; then \
		SETUP_CMD=$$(echo "$$TEST_SETUP" | sed "s|\$$(TEST_DATA)|$$ABS_TEST_DATA|g" | sed "s|\$$(TEST_TMPDIR)|$$TEST_TMPDIR|g" | sed "s|\$$(TEST_RESULTS_DIR)|$$TEST_RESULTS_DIR|g"); \
		echo "Setup: $$SETUP_CMD"; \
		eval "$$SETUP_CMD" || { echo "=== FAIL: $* (setup) ==="; exit 1; }; \
	fi; \
	EXPANDED_ARGS=$$(echo "$$TEST_ARGS" | sed "s|\$$(TEST_DATA)|$$ABS_TEST_DATA|g" | sed "s|\$$(TEST_TMPDIR)|$$TEST_TMPDIR|g"); \
	if [ -n "$$TEST_RUNNER" ]; then \
		cd $* && HEADLESS_BIN="$(VALGRIND) $(VALGRIND_FLAGS) $(abspath $(HEADLESS_BIN))" ROM_PATH="$$ROM_PATH" \
			TEST_DATA="$$ABS_TEST_DATA" TEST_TMPDIR="$$TEST_TMPDIR" \
			TEST_RESULTS_DIR="$$TEST_RESULTS_DIR" \
			STORAGE_CACHE=$(abspath $(STORAGE_CACHE)) \
			bash "$$TEST_RUNNER" || { echo "=== FAIL (valgrind): $* ==="; exit 1; }; \
	else \
		cd $* && GS_STORAGE_CACHE=$(abspath $(STORAGE_CACHE)) $(VALGRIND) $(VALGRIND_FLAGS) $(abspath $(HEADLESS_BIN)) \
			rom="$$ROM_PATH" \
			$$EXPANDED_ARGS \
			script=test.script \
			--speed=max || { echo "=== FAIL (valgrind): $* ==="; exit 1; }; \
	fi; \
	echo "=== PASS (valgrind): $* ==="