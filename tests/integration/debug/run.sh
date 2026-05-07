#!/bin/bash
# Integration test: Debug tooling — `find` family. Runs the headless
# emulator with test.script, captures stdout, and greps for expected
# markers. Each `find.<sub>` method dispatches into the legacy
# cmd_find_handler via shell_dispatch, so the parser's match-format
# and diagnostic output is unchanged.

# Note: no `set -e` — the test.script deliberately exercises error paths
# (bad pattern, bad range, non-hex tokens, out-of-range values), which
# can make the headless exit non-zero. We accept that exit status;
# verification comes from the post-hoc grep assertions below.

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
expect() {
    local needle="$1"
    local why="$2"
    if ! grep -F -q -- "$needle" "$LOG"; then
        echo "FAIL: expected marker not found: $needle  ($why)"
        rc=1
    fi
}
expect_not() {
    local needle="$1"
    local why="$2"
    if grep -F -q -- "$needle" "$LOG"; then
        echo "FAIL: unexpected marker present: $needle  ($why)"
        rc=1
    fi
}

# Introspection: methods("find") prints a JSON list of the four
# sub-command method names.
expect "str" "methods(\"find\") should list 'str'"
expect "bytes" "methods(\"find\") should list 'bytes'"
expect "word" "methods(\"find\") should list 'word'"
expect "long" "methods(\"find\") should list 'long'"

# Literal ASCII search should locate "Apple" in the Plus ROM mirror.
# Output format from cmd_find_handler is '$XXXXXXXX  "Apple"'.
expect '"Apple"' "find.str should emit match label in quotes"
expect '$0040A714' "find.str should locate 'Apple' at the known ROM offset"

# Hex-byte search should produce hits for the ubiquitous 68K NOP.
expect "  4E 71" "find.bytes should emit hex-label for '4E 71' hits"

# `find.word $4170` / `find.long $4170706C` should hit at the same ROM
# offset as the ASCII "Apple" hit at $0040A714, with numeric labels.
expect '$4170' "find.word should emit the reconstructed 16-bit literal as label"
expect '$4170706C' "find.long should emit the reconstructed 32-bit literal as label"
expect '$0040A714  $4170' "find.word should locate \$4170 at 'Apple' offset"
expect '$0040A714  $4170706C' "find.long should locate \$4170706C at 'Apple' offset"

# Error diagnostics from cmd_find_handler — must be clear, not silent.
# These come from the legacy parser through shell_dispatch and are
# unchanged by the typed wrapper.
expect "empty pattern" "'find.str \"\"' should reject empty pattern"
expect "expected 2-digit hex" "'find.bytes \"ZZ\"' should reject non-hex tokens"
expect "range end must be greater" "backwards range should error"
expect "exceeds 16 bits" "'find.word \$10000' should reject out-of-range value"
expect "must be INT, got STRING" "'find.word zzz' should reject non-numeric value"

# Well-formed `all` invocation must not surface a usage marker — i.e.
# the parser accepted both the pattern and the range without falling
# back to the help text.
expect_not "usage: find str" "well-formed 'find.str ... all' must not print legacy usage"
expect_not "usage: find bytes" "well-formed 'find.bytes ... all' must not print legacy usage"

if [ $rc -eq 0 ]; then
    echo "debug tooling integration checks passed"
else
    echo "debug tooling integration checks FAILED; see $LOG"
    tail -80 "$LOG" || true
fi
exit $rc
