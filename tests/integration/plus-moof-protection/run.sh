#!/bin/bash
# The plus-moof-protection row (see config.mk):
#   1. the emulator writes the System 6.0.8 disk as a MOOF (make.lua);
#   2. make-protected-moof.py puts the protection into its bits;
#   3. the Plus boots from it and test.lua runs the checks.
#
# Environment from the runner: HEADLESS_BIN, ROM_PATH, TEST_DATA, WORK_DIR,
# TEST_RESULTS_DIR, STORAGE_CACHE, TEST_VAR_ARGS.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLAIN="$WORK_DIR/plain.moof"
PROTECTED="$WORK_DIR/protected.moof"

cd "$WORK_DIR"
GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    script="$SCRIPT_DIR/make.lua" \
    --var SRC="$TEST_DATA/systems/System_6_0_8.dsk" \
    --var DST="$PLAIN" \
    --speed=turbo
[ -f "$PLAIN" ] || { echo "ERROR: no MOOF was written"; exit 1; }

python3 "$SCRIPT_DIR/make-protected-moof.py" "$PLAIN" "$PROTECTED"

cd "$SCRIPT_DIR"
# shellcheck disable=SC2086 — TEST_VAR_ARGS is intentionally word-split
GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    fd="$PROTECTED" \
    --checkpoint-dir="$WORK_DIR/cp" \
    script="$SCRIPT_DIR/test.lua" \
    --var MOOF="$PROTECTED" \
    --var WORK_DIR="$WORK_DIR" \
    $TEST_VAR_ARGS \
    --speed=turbo 2>&1 | tee "$TEST_RESULTS_DIR/output.log"
exit "${PIPESTATUS[0]}"
