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

# Typed find results: the script echoes stable markers derived from
# the returned V_LIST values ($0040A714 is where "Apple" lives in the
# Plus ROM mirror).
expect 'str-first=0040a714' "find.str first hit should be the known ROM offset"
expect 'word-first=0040a714' "find.word should hit at the 'Apple' offset"
expect 'long-first=0040a714' "find.long should hit at the 'Apple' offset"
expect 'nop-count=' "find.bytes should report its hit count"

# Error paths are exercised in-script with try(...) asserts; if any of
# them regressed, an ASSERT FAILED marker appears.
expect_not "ASSERT FAILED" "all in-script assertions must pass"

# Format specs in ${expr:fmt}. The literal $ is doubled here so the
# shell that runs run.sh doesn't expand the value before grep sees it.
expect "ab" "format spec :x should produce lowercase hex without prefix"
expect "000000ab" "format spec :08x should zero-pad to width 8"
expect "[      AB]" "format spec :8X should space-pad and uppercase"
expect "171" "format spec :d should print 0xab decimal"
expect "[   42]" "format spec :5d should space-pad"
expect "fmt-marker[000000ab]" "format spec must apply inside string interpolation"
expect_not "trailing garbage in expression" "format-spec colon must not leak into expr parser"

# debug.disasm with an explicit address (two-arg form). The address
# tag printed at the start of each line is the stable marker.
expect '$00400090' "debug.disasm <addr> <count> should disassemble at the given address"

if [ $rc -eq 0 ]; then
    echo "debug tooling integration checks passed"
else
    echo "debug tooling integration checks FAILED; see $LOG"
    tail -80 "$LOG" || true
fi
exit $rc
