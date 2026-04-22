#!/bin/bash
# Integration test: Debug tooling (PR1 of proposal-debug-tooling.md)
# Runs the headless emulator with test.script, captures stdout, and greps
# for expected markers to verify that `find str` / `find bytes` actually
# produce hits and diagnose bad input correctly.

# Note: no `set -e` — the test.script deliberately exercises error paths
# (bad pattern, bad range, unknown sub-command), which make the emulator
# exit non-zero.  We accept that exit status; verification comes from the
# post-hoc grep assertions below.

# Env supplied by the parent Makefile: HEADLESS_BIN, ROM_PATH, TEST_DATA,
# TEST_TMPDIR, TEST_RESULTS_DIR, WORK_DIR, STORAGE_CACHE

LOG="$TEST_RESULTS_DIR/debug.log"
mkdir -p "$TEST_RESULTS_DIR"

echo "Running headless with test.script; capturing output to $LOG"
GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    script=test.script \
    --speed=max \
    >"$LOG" 2>&1 || true

rc=0
# Assert helper: grep for a fixed-string needle; fail the test if missing.
expect() {
    local needle="$1"
    local why="$2"
    if ! grep -F -q -- "$needle" "$LOG"; then
        echo "FAIL: expected marker not found: $needle  ($why)"
        rc=1
    fi
}
# expect_not: fail the test if needle IS present (negative assertion).
expect_not() {
    local needle="$1"
    local why="$2"
    if grep -F -q -- "$needle" "$LOG"; then
        echo "FAIL: unexpected marker present: $needle  ($why)"
        rc=1
    fi
}

# Help output covers both sub-commands.
expect "find str <text>" "help find should document 'str' sub-command"
expect "find bytes" "help find should document 'bytes' sub-command"

# Literal ASCII search should locate "Apple" in the Plus ROM mirror.
# The output format is '$XXXXXXXX  "Apple"' so grep for both anchor forms.
expect '"Apple"' "find str should emit match label in quotes"
expect '$0040A714' "find str should locate 'Apple' at the known ROM offset"

# "<start> <count>" form exercises the alternative range parser path.
# With count=0x40000 from $400000 the same hit at $0040A714 should appear.
# (Already asserted via the label above; no additional marker needed.)

# Hex-byte search should produce hits for the ubiquitous 68K NOP.
expect "  4E 71" "find bytes should emit hex-label for '4E 71' hits"

# Error diagnostics — must be clear, not silent.
expect "usage: find" "bare 'find' with no sub-command should print usage"
expect "empty pattern" "'find str \"\"' should reject empty pattern"
expect "unknown subcommand" "'find bogus' should reject unknown subcommand"
expect "expected 2-digit hex" "'find bytes ZZ' should reject non-hex tokens"
expect "range end must be greater" "backwards range should error"

# The 'all' uncap path should not print the "... (N more" truncation marker.
# If `find str "Apple" ... all` emits that marker it means uncapping failed.
# (We can't easily isolate one command's output, but we can at least check
# that a well-formed 'all' invocation was accepted without a usage error.)
expect_not "usage: find str" "well-formed 'find str ... all' must not print usage"

if [ $rc -eq 0 ]; then
    echo "debug tooling integration checks passed"
else
    echo "debug tooling integration checks FAILED; see $LOG"
    tail -80 "$LOG" || true
fi
exit $rc
