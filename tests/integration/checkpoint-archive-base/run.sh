#!/bin/bash
# Integration test: checkpoint archive base (see config.mk).
#
# Environment from the runner: HEADLESS_BIN, ROM_PATH, TEST_RESULTS_DIR,
# STORAGE_CACHE, TEST_VAR_ARGS.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cd "$TEST_RESULTS_DIR"
# shellcheck disable=SC2086 — TEST_VAR_ARGS is intentionally word-split
out=$(GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    script="$SCRIPT_DIR/step1.lua" \
    --checkpoint-dir="$TEST_RESULTS_DIR/cp" \
    --var TEST_RESULTS_DIR="$TEST_RESULTS_DIR" \
    $TEST_VAR_ARGS \
    --speed=turbo 2>&1) || { echo "$out"; exit 1; }
echo "$out"
if echo "$out" | grep -q "Error:"; then
    echo "ERROR: the load reported an error"
    exit 1
fi
echo "Checkpoint archive base test passed"
