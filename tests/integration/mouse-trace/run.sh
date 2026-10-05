#!/bin/bash
# Mouse trace test (see config.mk).

set -e

# Set by the parent Makefile: HEADLESS_BIN, TEST_DATA, TEST_TMPDIR,
# STORAGE_CACHE.

ROM="$TEST_DATA/roms/plus-v3-4d1f8172.rom"
CP="$TEST_TMPDIR/mouse.gs"

# A Plus with no disk runs its ROM to the question-mark screen; 30 M
# instructions is past the ROM overlay, so low memory is RAM.  One emulated
# second is ~8 M instructions, so 20 M covers at least two trace ticks.
cat > "$TEST_TMPDIR/test.script" << EOF
scheduler.run 30000000
machine.adb.mouse.trace true
machine.adb.mouse.move 100 80 "global"
scheduler.run 20000000
echo "MARK first-machine"

# A new machine starts without the trace, and it can be turned on again.
machine.boot model="plus" rom="$ROM"
scheduler.run 30000000
echo "MARK fresh-machine"
machine.adb.mouse.trace true
machine.adb.mouse.move 120 90 "global"
scheduler.run 20000000
echo "MARK second-machine"

# Saved with the trace running; restored into a third machine without it.
assert checkpoint.save("$CP") "checkpoint save"
machine.boot model="plus" rom="$ROM"
scheduler.run 1000000
assert checkpoint.load("$CP") "checkpoint load"
echo "MARK restored"
machine.adb.mouse.move 10 10 "global"
scheduler.run 20000000
echo "MARK end"
quit
EOF

GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN rom="$ROM" model=plus script="$TEST_TMPDIR/test.script" \
    --speed=turbo > "$TEST_TMPDIR/out.log" 2>&1 || { cat "$TEST_TMPDIR/out.log"; exit 1; }

# Trace lines between each pair of markers.
traces() {
    awk -v a="$1" -v b="$2" '$0 == "MARK " a {on = 1; next} $0 == "MARK " b {on = 0} on && /^\[trace-mouse\]/' \
        "$TEST_TMPDIR/out.log"
}
fail() {
    echo "ERROR: $1"
    cat "$TEST_TMPDIR/out.log"
    exit 1
}

grep -q '^\[trace-mouse\] h=100 v=80$' "$TEST_TMPDIR/out.log" || fail "no trace on the first machine"
[ -z "$(traces first-machine fresh-machine)" ] || fail "the trace survived machine.boot"
traces fresh-machine second-machine | grep -q '^\[trace-mouse\] h=120 v=90$' || fail "mouse.trace true after machine.boot printed nothing"
traces restored end | grep -q '^\[trace-mouse\] h=10 v=10$' || fail "the trace did not restore from the checkpoint"
grep -q '^MARK end$' "$TEST_TMPDIR/out.log" || fail "the script did not finish"
echo "mouse-trace: trace re-enabled after a boot and restored from a checkpoint"
rm -f "$CP"
