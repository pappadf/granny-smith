#!/bin/bash
# Marathon 2 across routers (see config.mk): A gathers, B joins.
#
# Set by the runner: HEADLESS_BIN, ROM_PATH, TEST_DATA, TEST_TMPDIR, WORK_DIR,
# STORAGE_CACHE, TEST_VAR_ARGS.  The runner starts us in the test directory,
# so A's script finds ../lib, the helpers and its golden here.
#
# Both instances run without a pause from start to end: a guest whose
# instance stops running stops answering, and the other's ADSP open (the
# map) or the game's ring times out.

set -u

IMG="$TEST_DATA/apps/marathon_12_pm8100.img"
if [ ! -f "$IMG" ]; then
    echo "skip: apps/marathon_12_pm8100.img is not in the fetched test data"
    echo "@@SKIP apps/marathon_12_pm8100.img is not in the fetched test data" | tee -a "$TEST_RESULTS_DIR/output.log"
    exit 0
fi
cp "$IMG" "$TEST_TMPDIR/a.img" && cp "$IMG" "$TEST_TMPDIR/b.img" || exit 1

BASE=$((20000 + RANDOM % 30000))
L=127.0.0.1
HERE=$(pwd)

cat > "$TEST_TMPDIR/b.script" << EOS
include "$HERE/marathon.script"
let ROM = "$ROM_PATH"
appletalk.router.net = 302
appletalk.router.name = "B"
appletalk.router.link_udp("$L:$((BASE + 1))", "$L:$BASE", false)
m2_boot_to_menu("$TEST_TMPDIR/b.img")
pacing.mode = "paced"
# Join Network Game, as Carol: "Now waiting to be gathered"
m2_click(135, 317)
machine.adb.keyboard.type "Carol"
scheduler.run 300000000
machine.adb.keyboard.press "return"
# Until A gathers this player and the game runs
m2_wait_game_traffic(100, 3, 1000000000, 120000000000)
machine.screen.save "$TEST_RESULTS_DIR/b-game.png"
echo "B: in the game, \${appletalk.router.stats.forwarded_out} datagrams sent to A's network"
echo "B OK"
# Play on until A is done: it closes the link when it is
let j = 0
while \$j < 2000 && appletalk.router.links[0].state != "closed" {
    scheduler.run 10000000
    \$j = \$j + 1
}
quit
EOS

cat > "$TEST_TMPDIR/a.script" << EOS
include "$HERE/../lib/mac.script"
include "$HERE/marathon.script"
let ROM = "$ROM_PATH"
appletalk.router.net = 301
appletalk.router.name = "A"
appletalk.router.link_udp("$L:$BASE", "$L:$((BASE + 1))", false)
m2_boot_to_menu("$TEST_TMPDIR/a.img")
pacing.mode = "paced"
# Gather Network Game, as Alice: the setup dialog, then Gather
m2_click(117, 275)
machine.adb.keyboard.type "Alice"
scheduler.run 300000000
machine.adb.keyboard.press "return"
# Carol, on B's network, is listed: found through both routers
run_until_match("gather-carol.png", 60000000000, 200000000)
check("gather-carol.png")
assert appletalk.router.stats.nbp_fwdreq_out > 0 "Gather's lookups were not forwarded to B"
# Add Carol and start the game
m2_click(100, 123)
m2_click(271, 287)
m2_click(539, 287)
m2_wait_game_traffic(100, 3, 1000000000, 60000000000)
machine.screen.save "$TEST_RESULTS_DIR/a-game.png"
echo "A: in the game, \${appletalk.router.stats.forwarded_out} datagrams sent to B's network"
echo "A OK"
appletalk.router.links[0].close()
quit
EOS

run() { # name, extra args...
    local n=$1
    shift
    GS_STORAGE_CACHE="$STORAGE_CACHE/$n" $HEADLESS_BIN rom="$ROM_PATH" "$@" script="$TEST_TMPDIR/$n.script" \
        --var WORK_DIR="$WORK_DIR" $TEST_VAR_ARGS > "$TEST_TMPDIR/$n.log" 2>&1
}

mkdir -p "$STORAGE_CACHE/a" "$STORAGE_CACHE/b"
run b --speed=turbo & PB=$!
run a --speed=turbo & PA=$!
# The first to fail stops the other: it would wait for it to the end
rc=0
for _ in 1 2; do
    if ! wait -n; then
        rc=1
        kill $PA $PB 2>/dev/null
    fi
done
for n in a b; do
    echo "--- $n ---"
    grep -E '^[AB]:|OK$|MATCH|ASSERT|rror' "$TEST_TMPDIR/$n.log" || true
done
grep -q "^A OK" "$TEST_TMPDIR/a.log" || { echo "ERROR: A did not finish"; rc=1; }
grep -q "^B OK" "$TEST_TMPDIR/b.log" || { echo "ERROR: B did not finish"; rc=1; }
if [ $rc -ne 0 ]; then
    for n in a b; do
        echo "=== $n.log (tail) ==="
        grep -v '^# running' "$TEST_TMPDIR/$n.log" | tail -30
    done
fi
exit $rc
