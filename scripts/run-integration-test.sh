#!/usr/bin/env bash
# run-integration-test.sh — run one integration test directory.
#
# Extracted from the tests/integration Makefile pattern rules so the
# runner logic exists exactly once: the plain and valgrind flavors
# differ only in the WRAPPER variable. Invoked with CWD = tests/integration and the test directory
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
#   GS_EXTRA_MEDIA_DIR  optional: a directory of media that is not
#                     redistributable and so not in the test data.  Scripts
#                     see it as $EXTRA_MEDIA and skip rows whose media is
#                     absent; unset, it names a directory that cannot exist.
#
# A TEST_RUNNER script additionally receives TEST_VAR_ARGS — TEST_VARS
# pre-formatted as "--var K=V ..." — to splice into its own emulator
# invocations so multi-process tests honor ROW=/REGEN= too.
#
# Each test gets a private GS_STORAGE_CACHE under its WORK_DIR: the
# binary routes every delta/journal/scratch sidecar there, so nothing
# writes into tests/data and independent tests can run in parallel.

set -euo pipefail

TEST="${1:?usage: run-integration-test.sh <test-dir>}"
LABEL="${WRAPPER:+ (valgrind)}"

# Any command failing outside an explicit check is a runner error: say
# so and record it, rather than exiting silently with no status file.
trap 'echo "=== FAIL${LABEL}: $TEST (runner error at line $LINENO) ==="; echo FAIL > "${TEST_RESULTS_DIR:-/nonexistent}/status" 2>/dev/null || true' ERR

fail() {
    echo "=== FAIL${LABEL}: $TEST ==="
    echo "FAIL" > "$TEST_RESULTS_DIR/status" 2>/dev/null
    exit 1
}

[ -d "$TEST" ] || { echo "ERROR: Test directory '$TEST' not found"; exit 1; }
[ -f "$TEST/test.script" ] || { echo "ERROR: Test script '$TEST/test.script' not found"; exit 1; }
[ -f "$TEST/config.mk" ] || { echo "ERROR: Test config '$TEST/config.mk' not found"; exit 1; }

# Extract configuration from config.mk: the first `KEY :=` line, exactly that
# key (TEST_SETUP never matches TEST_SETUP_EXTRA), empty when absent.
cfg() { sed -n "/^$1[[:space:]]*:=/{s/^[^=]*=[[:space:]]*//p;q}" "$TEST/config.mk"; }
TEST_ROM=$(cfg TEST_ROM)
TEST_ARGS=$(cfg TEST_ARGS)
TEST_SETUP=$(cfg TEST_SETUP)
TEST_RUNNER=$(cfg TEST_RUNNER)
TEST_NAME=$(cfg TEST_NAME)

# A hang outside any in-script wait (a daemon that never starts, an unbounded
# run) must end the test, not the CI job.  900 s is about twice the slowest
# native test; under a wrapper (Valgrind: 20-50x) the slowest unit-tier test
# takes about 1450 s, so the ceiling is an hour.  A config may set its own.
TEST_TIMEOUT=$(cfg TEST_TIMEOUT)
TEST_TIMEOUT=${TEST_TIMEOUT:-$([ -n "${WRAPPER:-}" ] && echo 3600 || echo 900)}

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
EXTRA_MEDIA="${GS_EXTRA_MEDIA_DIR:-$WORK_DIR/no-extra-media}"

# Substitute the Makefile-style placeholders used in config.mk values.  Plain
# parameter expansion, so a path holding '&', '|' or '\' arrives verbatim (the
# replacement is quoted, which also turns off bash 5.2's '&' substitution).
expand() {
    local s=$1
    s=${s//'$(TEST_DATA)'/"$TEST_DATA"}
    s=${s//'$(TEST_TMPDIR)'/"$TEST_TMPDIR"}
    s=${s//'$(TEST_RESULTS_DIR)'/"$TEST_RESULTS_DIR"}
    s=${s//'$(WORK_DIR)'/"$WORK_DIR"}
    printf '%s\n' "$s"
}

# Report a timeout (exit 124 from timeout(1)) distinctly from a failure.
check_rc() {
    local rc=$1
    [ "$rc" -eq 0 ] && return 0
    if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        echo "=== FAIL${LABEL}: $TEST (timeout after ${TEST_TIMEOUT}s) ==="
        echo "FAIL" > "$TEST_RESULTS_DIR/status" 2>/dev/null
        exit 1
    fi
    fail
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

# Under a wrapper, hand custom runners a real executable rather than a
# multi-word "valgrind --flags... /path/to/bin" string: every run.sh
# quotes "$HEADLESS_BIN" (as it must, paths may contain spaces), which
# turned the folded form into one nonexistent filename and failed the
# test before it ran. A shim keeps HEADLESS_BIN a single path and moves
# the wrapper inside it.
RUNNER_BIN="$HEADLESS_BIN"
if [ -n "${WRAPPER:-}" ]; then
    SHIM="$TEST_TMPDIR/headless-wrapped"
    {
        echo '#!/usr/bin/env bash'
        echo "exec $WRAPPER \"\$HEADLESS_REAL_BIN\" \"\$@\""
    } > "$SHIM"
    chmod +x "$SHIM"
    export HEADLESS_REAL_BIN="$HEADLESS_BIN"
    RUNNER_BIN="$SHIM"
fi

cd "$TEST" || exit 1
if [ -n "$TEST_RUNNER" ]; then
    # Custom multi-step runner: it invokes $HEADLESS_BIN itself. Under a
    # wrapper it gets the shim path, so its quoted "$HEADLESS_BIN" works.
    HEADLESS_BIN="$RUNNER_BIN" ROM_PATH="$ROM_PATH" \
        DUMP_BIN="$DUMP_BIN" \
        TEST_DATA="$TEST_DATA" TEST_TMPDIR="$TEST_TMPDIR" \
        EXTRA_MEDIA="$EXTRA_MEDIA" \
        TEST_RESULTS_DIR="$TEST_RESULTS_DIR" \
        WORK_DIR="$WORK_DIR" \
        STORAGE_CACHE="$GS_STORAGE_CACHE" \
        TEST_VAR_ARGS="$VAR_ARGS" \
        timeout -k 30 "$TEST_TIMEOUT" bash "$TEST_RUNNER" || rc=$?
    check_rc "${rc:-0}"
else
    # shellcheck disable=SC2086 — args and vars are intentionally word-split
    # $ROM mirrors the startup rom= so scripts can re-boot with an explicit
    # rom="${$ROM}" (machine.boot inherits nothing from the running machine).
    # $TEST_DATA is the fixture root, for rows that name a second file by
    # path (a card's expansion ROM, a second disk image).
    timeout -k 30 "$TEST_TIMEOUT" ${WRAPPER:-} "$HEADLESS_BIN" \
        rom="$ROM_PATH" \
        $EXPANDED_ARGS \
        script=test.script \
        --var WORK_DIR="$WORK_DIR" \
        --var TEST_RESULTS_DIR="$TEST_RESULTS_DIR" \
        --var ROM="$ROM_PATH" \
        --var TEST_DATA="$TEST_DATA" \
        --var EXTRA_MEDIA="$EXTRA_MEDIA" \
        $VAR_ARGS \
        --speed=max || rc=$?
    check_rc "${rc:-0}"
fi

echo "=== PASS${LABEL}: $TEST ==="
echo "PASS" > "$TEST_RESULTS_DIR/status"
