#!/bin/bash
# AppleTalk router links (see config.mk): three processes, A, B and C.
#
# Set by the runner: HEADLESS_BIN, ROM_PATH, TEST_TMPDIR, STORAGE_CACHE.

set -u

# Six UDP ports on the loopback interface, two per link, from a random base
# so parallel runs rarely meet.
BASE=$((20000 + RANDOM % 30000))
P_AB=$BASE P_BA=$((BASE + 1)) P_AC=$((BASE + 2)) P_CA=$((BASE + 3)) P_BC=$((BASE + 4)) P_CB=$((BASE + 5))
L=127.0.0.1

# The common part of each instance's script: its network, zone and name,
# and two helpers over the routing table.
prelude() { # net zone name
    cat << EOS
appletalk.router.net = $1
appletalk.router.zone = "$2"
appletalk.router.name = "$3"
# The distance to network \$n, or -1 while the router has no usable route
# (good, or suspect: not refreshed since the last validity tick)
def dist(n) {
    let st = try(appletalk.router.routes[\$n].state, "")
    if \$st == "good" || \$st == "suspect" {
        return appletalk.router.routes[\$n].distance
    }
    return -1
}
def zone(n) {
    return try(appletalk.router.routes[\$n].zone, "")
}
# Run until network \$n is at distance \$d with zone \$z, at most \$limit times
def wait_route(n, d, z, limit) {
    let i = 0
    while \$i < \$limit && (dist(\$n) != \$d || zone(\$n) != \$z) {
        scheduler.run 1000000
        \$i = \$i + 1
    }
    assert dist(\$n) == \$d "network \${\$n}: distance \${dist(\$n)}, expected \${\$d}"
    assert zone(\$n) == \$z "network \${\$n}: zone '\${zone(\$n)}', expected '\${\$z}'"
}
# The end: say this instance is done, then keep its router up until every
# instance is, so none loses a peer it still needs
def finish(me) {
    files.mkdir("$TEST_TMPDIR/done-\${\$me}")
    let i = 0
    while \$i < 100000 && (try(files.ls("$TEST_TMPDIR/done-a"), "no") == "no" || try(files.ls("$TEST_TMPDIR/done-b"), "no") == "no" || try(files.ls("$TEST_TMPDIR/done-c"), "no") == "no") {
        scheduler.run 1000000
        \$i = \$i + 1
    }
}
EOS
}

cat > "$TEST_TMPDIR/a.script" << EOS
$(prelude 101 "Zone A" "A")
appletalk.router.link_udp("$L:$P_AB", "$L:$P_BA")
appletalk.router.link_udp("$L:$P_AC", "$L:$P_CA")
wait_route("102", 1, "Zone B", 20000)
wait_route("103", 1, "Zone C", 20000)
echo "A: triangle converged"
files.mkdir("$TEST_TMPDIR/converged-a")
# C cuts its link to A: network 103 is now two hops away, through B
wait_route("103", 2, "Zone C", 20000)
echo "A: rerouted to 103 through B"
assert appletalk.router.links[1].state == "closed" "A's link to C is not closed"
echo "A OK"
finish("a")
quit
EOS

cat > "$TEST_TMPDIR/b.script" << EOS
$(prelude 102 "Zone B" "B")
appletalk.router.link_udp("$L:$P_BA", "$L:$P_AB")
appletalk.router.link_udp("$L:$P_BC", "$L:$P_CB")
wait_route("101", 1, "Zone A", 20000)
wait_route("103", 1, "Zone C", 20000)
echo "B: triangle converged"
# B keeps both its links through the cut (it carries A <-> C traffic after),
# checked while A and C still run; then it stays up while they finish
scheduler.run 20000000
assert dist("101") == 1 "B lost network 101"
assert dist("103") == 1 "B lost network 103"
echo "B OK"
finish("b")
quit
EOS

cat > "$TEST_TMPDIR/c.script" << EOS
$(prelude 103 "Zone C" "C")
appletalk.router.link_udp("$L:$P_CA", "$L:$P_AC")
appletalk.router.link_udp("$L:$P_CB", "$L:$P_BC")
wait_route("101", 1, "Zone A", 20000)
wait_route("102", 1, "Zone B", 20000)
echo "C: triangle converged"
# Wait for A to converge too (its marker), then cut the link to A
files.mkdir("$TEST_TMPDIR/converged-c")
let w = 0
while \$w < 100000 && try(files.ls("$TEST_TMPDIR/converged-a"), "no") == "no" {
    scheduler.run 1000000
    \$w = \$w + 1
}
appletalk.router.links[0].close()
wait_route("101", 2, "Zone A", 20000)
echo "C: rerouted to 101 through B"
echo "C OK"
finish("c")
quit
EOS

run() { # name
    GS_STORAGE_CACHE="$STORAGE_CACHE/$1" $HEADLESS_BIN rom="$ROM_PATH" script="$TEST_TMPDIR/$1.script" \
        > "$TEST_TMPDIR/$1.log" 2>&1
}

mkdir -p "$STORAGE_CACHE/a" "$STORAGE_CACHE/b" "$STORAGE_CACHE/c"
run a & PA=$!
run b & PB=$!
run c & PC=$!
# The first to fail stops the others: they would wait for it to the end
rc=0
for _ in 1 2 3; do
    if ! wait -n; then
        rc=1
        kill $PA $PB $PC 2>/dev/null
    fi
done

for n in a b c; do
    echo "--- $n ---"
    grep -E '^[ABC]:|OK$|ASSERT|rror' "$TEST_TMPDIR/$n.log" || true
done
for n in A B C; do
    lower=$(echo "$n" | tr A-Z a-z)
    grep -q "^$n OK" "$TEST_TMPDIR/$lower.log" || { echo "ERROR: $n did not finish"; rc=1; }
done
if [ $rc -ne 0 ]; then
    for n in a b c; do
        echo "=== $n.log (tail) ==="
        tail -25 "$TEST_TMPDIR/$n.log"
    done
fi
exit $rc
