#!/usr/bin/env bash
# run-integration-test.sh — run one integration test directory.
#
# Extracted from the tests/integration Makefile pattern rules so the
# runner logic exists exactly once: the plain and valgrind flavors
# differ only in the WRAPPER variable (proposal-integration-test-rework
# §5.3). Invoked with CWD = tests/integration and the test directory
# name as $1.
#
# Environment (absolute paths, exported by the Makefile):
#   HEADLESS_BIN      emulator binary
#   DUMP_BIN          dump tool (re-* tests)
#   TEST_DATA         tests/data
#   TEST_RESULTS_DIR  per-test results dir (created fresh here)
#   WORK_DIR          per-test scratch dir (created fresh here)
#   WRAPPER           optional command prefix (e.g. "valgrind --quiet ...")
#   TEST_VARS         optional extra shell --var definitions ("ROW=x REGEN=1")
#
# A TEST_RUNNER script additionally receives TEST_VAR_ARGS — TEST_VARS
# pre-formatted as "--var K=V ..." — to splice into its own emulator
# invocations so multi-process tests honor ROW=/REGEN= too.
#
# Each test gets a private GS_STORAGE_CACHE under its WORK_DIR: the
# binary routes every delta/journal/scratch sidecar there, so nothing
# writes into tests/data and independent tests can run in parallel.

set -u

TEST="${1:?usage: run-integration-test.sh <test-dir>}"
LABEL="${WRAPPER:+ (valgrind)}"

fail() {
    echo "=== FAIL${LABEL}: $TEST ==="
    echo "FAIL" > "$TEST_RESULTS_DIR/status" 2>/dev/null
    exit 1
}

[ -d "$TEST" ] || { echo "ERROR: Test directory '$TEST' not found"; exit 1; }
[ -f "$TEST/test.script" ] || { echo "ERROR: Test script '$TEST/test.script' not found"; exit 1; }
[ -f "$TEST/config.mk" ] || { echo "ERROR: Test config '$TEST/config.mk' not found"; exit 1; }

# Extract configuration from config.mk (first match per key).
cfg() { grep -m1 "^$1" "$TEST/config.mk" | cut -d= -f2- | sed 's/^[ \t]*//'; }
TEST_ROM=$(cfg TEST_ROM)
TEST_ARGS=$(cfg TEST_ARGS)
TEST_SETUP=$(cfg TEST_SETUP)
TEST_RUNNER=$(cfg TEST_RUNNER)
TEST_NAME=$(cfg TEST_NAME)

echo "=== Running${LABEL}: $TEST_NAME ($TEST) ==="

ROM_PATH="$TEST_DATA/$TEST_ROM"
[ -f "$ROM_PATH" ] || { echo "ERROR: ROM not found: $ROM_PATH"; exit 1; }

# Fresh per-test directories; the storage cache lives inside WORK_DIR so
# it shares its lifetime and never collides with a concurrent test.
TEST_TMPDIR=$(mktemp -d)
trap 'rm -rf "$TEST_TMPDIR"' EXIT
rm -rf "$WORK_DIR"
mkdir -p "$TEST_RESULTS_DIR" "$WORK_DIR"
rm -f "$TEST_RESULTS_DIR/status"
export GS_STORAGE_CACHE="$WORK_DIR/storage-cache"

# Substitute the Makefile-style placeholders used in config.mk values.
expand() {
    echo "$1" | sed "s|\$(TEST_DATA)|$TEST_DATA|g" \
               | sed "s|\$(TEST_TMPDIR)|$TEST_TMPDIR|g" \
               | sed "s|\$(TEST_RESULTS_DIR)|$TEST_RESULTS_DIR|g" \
               | sed "s|\$(WORK_DIR)|$WORK_DIR|g"
}

if [ -n "$TEST_SETUP" ]; then
    SETUP_CMD=$(expand "$TEST_SETUP")
    echo "Setup: $SETUP_CMD"
    eval "$SETUP_CMD" || { echo "=== FAIL${LABEL}: $TEST (setup) ==="; echo "FAIL" > "$TEST_RESULTS_DIR/status"; exit 1; }
fi

EXPANDED_ARGS=$(expand "$TEST_ARGS")

# Optional extra shell variables (ROW=, REGEN=, KEEP_GOING=, ...).
VAR_ARGS=""
for v in ${TEST_VARS:-}; do
    VAR_ARGS="$VAR_ARGS --var $v"
done

cd "$TEST" || exit 1
if [ -n "$TEST_RUNNER" ]; then
    # Custom multi-step runner: it invokes $HEADLESS_BIN itself, with the
    # wrapper folded into the variable (the historical valgrind contract).
    HEADLESS_BIN="${WRAPPER:+$WRAPPER }$HEADLESS_BIN" ROM_PATH="$ROM_PATH" \
        DUMP_BIN="$DUMP_BIN" \
        TEST_DATA="$TEST_DATA" TEST_TMPDIR="$TEST_TMPDIR" \
        TEST_RESULTS_DIR="$TEST_RESULTS_DIR" \
        WORK_DIR="$WORK_DIR" \
        STORAGE_CACHE="$GS_STORAGE_CACHE" \
        TEST_VAR_ARGS="$VAR_ARGS" \
        bash "$TEST_RUNNER" || fail
else
    # shellcheck disable=SC2086 — args and vars are intentionally word-split
    ${WRAPPER:-} "$HEADLESS_BIN" \
        rom="$ROM_PATH" \
        $EXPANDED_ARGS \
        script=test.script \
        --var WORK_DIR="$WORK_DIR" \
        --var TEST_RESULTS_DIR="$TEST_RESULTS_DIR" \
        $VAR_ARGS \
        --speed=max || fail
fi

echo "=== PASS${LABEL}: $TEST ==="
echo "PASS" > "$TEST_RESULTS_DIR/status"
