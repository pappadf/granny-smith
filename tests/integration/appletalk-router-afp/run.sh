#!/bin/bash
# AFP across routers (see config.mk): A has the guest, B the file server.
#
# Set by the runner: HEADLESS_BIN, ROM_PATH, TEST_TMPDIR, WORK_DIR,
# STORAGE_CACHE, TEST_VAR_ARGS.  The runner starts us in the test directory,
# so A's script finds ../lib and its goldens here.

set -u

BASE=$((20000 + RANDOM % 30000))
L=127.0.0.1
HERE=$(pwd)

cat > "$TEST_TMPDIR/b.script" << EOS
appletalk.router.net = 202
appletalk.router.name = "B"
appletalk.router.share_host_services = true
appletalk.afp.name = "Friend's Folders"
assert appletalk.afp.volumes.add("Friend Volume", "$WORK_DIR/share").path == "$WORK_DIR/share" "share not registered"
appletalk.router.link_udp("$L:$((BASE + 1))", "$L:$BASE", true)
# Serve until A's guest has mounted the volume
let i = 0
while \$i < 100000 && try(appletalk.afp.volumes["Friend Volume"].sessions_using, 0) < 1 {
    scheduler.run 1000000
    \$i = \$i + 1
}
assert appletalk.afp.volumes["Friend Volume"].sessions_using == 1 "A's guest did not mount the volume"
assert appletalk.afp.sessions[0].afp_version == "AFPVersion 2.0" "the session is not System 6's AFP 2.0"
assert appletalk.router.stats.nbp_fwdreq_in > 0 "no lookup was forwarded to this network"
assert appletalk.router.stats.forwarded_out > 0 "nothing went back to A's network"
echo "B: A's guest mounted 'Friend Volume' over the link"
echo "B OK"
# Stay up until A is done: it closes the link when it is
let j = 0
while \$j < 100000 && appletalk.router.links[0].state != "closed" {
    scheduler.run 1000000
    \$j = \$j + 1
}
quit
EOS

cat > "$TEST_TMPDIR/a.script" << EOS
include "$HERE/../lib/mac.script"
include "$HERE/../lib/afp.script"
machine.rtc.time = "2026-04-26T21:30:36"
appletalk.router.net = 201
appletalk.router.name = "A"
# Only B's server is to be found: this host's own is off
appletalk.afp.enabled = false
appletalk.router.link_udp("$L:$BASE", "$L:$((BASE + 1))", true)
scheduler.run 30000000
let i = 0
while \$i < 100000 && try(appletalk.router.routes["202"].zone, "") != "Granny Smith" {
    scheduler.run 1000000
    \$i = \$i + 1
}
assert try(appletalk.router.routes["202"].zone, "") == "Granny Smith" "B's network is not known in the shared zone"
# Chooser -> AppleShare: the server listed is B's, found through both routers
afp_chooser_to_volumes("chooser-friends-server.png", "")
afp_mount_and_close()
assert appletalk.router.stats.nbp_fwdreq_out > 0 "the Chooser's lookup was not forwarded to B"
assert appletalk.router.stats.forwarded_out > 20 "the session's datagrams did not go through the router"
check("desktop-mounted.png")
echo "A: mounted B's volume"
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
run b & PB=$!
run a config=appletalk-inactive.json hd="$TEST_TMPDIR/hd.img" & PA=$!
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
        tail -30 "$TEST_TMPDIR/$n.log"
    done
fi
exit $rc
