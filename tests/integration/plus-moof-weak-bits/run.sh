#!/bin/bash
# The plus-moof-weak-bits row (see config.mk):
#   1. the emulator writes the System 6.0.8 disk as a MOOF (make.lua);
#   2. make-weak-moof.py puts the weak area into a copy's bits;
#   3. the Plus boots from the plain MOOF with the weak one in drive 1,
#      and test.lua runs the check;
#   4. a second process runs the same reads and must print the same digest.
#
# Environment from the runner: HEADLESS_BIN, ROM_PATH, TEST_DATA, WORK_DIR,
# TEST_RESULTS_DIR, STORAGE_CACHE, TEST_VAR_ARGS.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLAIN="$WORK_DIR/plain.moof"
WEAK="$WORK_DIR/weak.moof"

cd "$WORK_DIR"
GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    script="$SCRIPT_DIR/make.lua" \
    --var SRC="$TEST_DATA/systems/System_6_0_8.dsk" \
    --var DST="$PLAIN" \
    --speed=turbo
[ -f "$PLAIN" ] || { echo "ERROR: no MOOF was written"; exit 1; }
python3 "$SCRIPT_DIR/make-weak-moof.py" "$PLAIN" "$WEAK"

# Boot from the plain MOOF (a copy per run: the Finder writes to it), the
# weak one in the external drive.
run() {
    cp "$PLAIN" "$WORK_DIR/boot-$1.moof"
    # shellcheck disable=SC2086 — TEST_VAR_ARGS is intentionally word-split
    GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
        rom="$ROM_PATH" \
        fd0="$WORK_DIR/boot-$1.moof" \
        fd1="$WEAK" \
        --checkpoint-dir="$WORK_DIR/cp-$1" \
        script="$SCRIPT_DIR/test.lua" \
        --var WORK_DIR="$WORK_DIR" \
        --var DIGEST_ONLY="$2" \
        $TEST_VAR_ARGS \
        --speed=turbo
}

cd "$SCRIPT_DIR"
run 1 0 2>&1 | tee "$TEST_RESULTS_DIR/output.log"
rc="${PIPESTATUS[0]}"
[ "$rc" -eq 0 ] || exit "$rc"
grep -q "weak-bits-ok" "$TEST_RESULTS_DIR/output.log" || { echo "ERROR: the check did not finish"; exit 1; }

# The same reads in another process: the same draws, nibble for nibble.
out=$(run 2 1 2>&1) || { echo "$out"; exit 1; }
first=$(grep "^reads: " "$TEST_RESULTS_DIR/output.log")
second=$(echo "$out" | grep "^reads: ")
echo "second process: $second"
[ -n "$first" ] && [ "$first" = "$second" ] || {
    echo "ERROR: another process read the weak area differently"
    echo "  first:  $first"
    echo "  second: $second"
    exit 1
}
echo "the same reads in a second process"
