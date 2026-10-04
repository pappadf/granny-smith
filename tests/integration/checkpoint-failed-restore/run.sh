#!/bin/bash
# Failed-restore test (see config.mk).

set -e

# Set by the parent Makefile: HEADLESS_BIN, TEST_DATA, TEST_TMPDIR,
# STORAGE_CACHE.

ROMS="$TEST_DATA/roms"
STATE='echo "STATE pc=${machine.cpu.pc} cycles=${scheduler.cycles} instr=${scheduler.instr_count} label=${machine.meta.label}"'

run() {
    GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN rom="$2" model="$1" script="$3" --speed=turbo
}

# model rom
failed_restore() {
    local model=$1 rom=$2
    local cp="$TEST_TMPDIR/$model.gs" cut="$TEST_TMPDIR/$model-cut.gs"

    cat > "$TEST_TMPDIR/save.script" << EOS
scheduler.run 3000000
assert checkpoint.save("$cp") "$model: save failed"
quit
EOS
    run "$model" "$rom" "$TEST_TMPDIR/save.script" > "$TEST_TMPDIR/save.log" 2>&1
    cp "$cp" "$cut"
    truncate -s -8 "$cut"

    cat > "$TEST_TMPDIR/control.script" << EOS
scheduler.run 2000000
scheduler.run 2000000
$STATE
quit
EOS
    cat > "$TEST_TMPDIR/failed.script" << EOS
scheduler.run 2000000
assert !checkpoint.load("$cut") "$model: the cut checkpoint loaded"
scheduler.run 2000000
$STATE
quit
EOS
    local a b
    a=$(run "$model" "$rom" "$TEST_TMPDIR/control.script" | grep '^STATE')
    b=$(run "$model" "$rom" "$TEST_TMPDIR/failed.script" | grep '^STATE')
    echo "$model control: $a"
    echo "$model failed:  $b"
    if [ -z "$a" ] || [ "$a" != "$b" ]; then
        echo "ERROR: $model: a failed restore changed the running machine"
        exit 1
    fi
    rm -f "$cp" "$cut"
}

failed_restore plus "$ROMS/plus-v3-4d1f8172.rom"
failed_restore se30 "$ROMS/iix-iicx-se30-97221136.rom"
failed_restore iici "$ROMS/iici-368cadfe.rom"
failed_restore q700 "$ROMS/q700-q900-420dbff3.rom"
failed_restore pm7100 "$ROMS/pm6100-pm7100-pm8100-9feb69b3.rom"
failed_restore pm9500 "$ROMS/pm7500-pm8500-pm9500-96cd923d.rom"
failed_restore lisa "$ROMS/lisa2-revh-098917b2.rom"

# What a failed restore must not touch besides the machine's run: the object
# tree the running machine projects (its expansion bus's slots, the screen's
# source, the root and machine nodes), and the memory-logpoint hook its
# debugger owns -- a logpoint set before the failed load keeps firing.
# model rom bus 68k (the 9500 boots with no display card, and a memory
# logpoint is a 68K slow-path hook)
failed_restore_tree() {
    local model=$1 rom=$2 bus=$3 m68k=$4
    local src_before="" src_after=""
    local lp_arm="" lp_check=""
    if [ "$m68k" = yes ]; then
        src_before="assert machine.screen.source \"$model: no screen source before the load\""
        src_after="assert machine.screen.source \"$model: a failed restore dropped screen.source\""
        lp_arm='debug.logpoints.add addr=0x16A width=l mode=write message="ticks"
scheduler.run 2000000
let hits_before = debug.logpoints.entries[0].hit_count
assert $hits_before > 0 "the Ticks logpoint never fired"'
        lp_check='scheduler.run 2000000
assert debug.logpoints.entries[0].hit_count > $hits_before "the logpoint stopped firing after a failed restore"'
    fi
    local cp="$TEST_TMPDIR/$model-tree.gs" cut="$TEST_TMPDIR/$model-tree-cut.gs"
    cat > "$TEST_TMPDIR/tree-save.script" << EOS
scheduler.run 3000000
assert checkpoint.save("$cp") "$model: save failed"
quit
EOS
    run "$model" "$rom" "$TEST_TMPDIR/tree-save.script" > /dev/null 2>&1
    cp "$cp" "$cut"
    truncate -s -8 "$cut"
    cat > "$TEST_TMPDIR/tree.script" << EOS
scheduler.run 8000000
let root_before = "\${meta.children}"
let machine_before = "\${machine.meta.children}"
let slots_before = machine.$bus.slot.count
assert \$slots_before > 0 "$model: no populated $bus slot to watch"
$src_before
$lp_arm
assert !checkpoint.load("$cut") "$model: the cut checkpoint loaded"
assert "\${meta.children}" == \$root_before "$model: a failed restore changed the root's children"
assert "\${machine.meta.children}" == \$machine_before "$model: a failed restore changed machine's children"
assert machine.$bus.slot.count == \$slots_before "$model: a failed restore dropped machine.$bus's slots"
$src_after
$lp_check
echo "TREE ok"
quit
EOS
    if ! run "$model" "$rom" "$TEST_TMPDIR/tree.script" > "$TEST_TMPDIR/tree.log" 2>&1 ||
        ! grep -q '^TREE ok' "$TEST_TMPDIR/tree.log"; then
        cat "$TEST_TMPDIR/tree.log"
        echo "ERROR: $model: a failed restore changed what the running machine projects"
        exit 1
    fi
    echo "$model: what it projects is intact"
    rm -f "$cp" "$cut"
}

failed_restore_tree iicx "$ROMS/iix-iicx-se30-97221136.rom" nubus yes
failed_restore_tree pm9500 "$ROMS/pm7500-pm8500-pm9500-96cd923d.rom" pci no

# Typed keys in flight: the keyboard's paced key events are pending in the
# queue when the checkpoint is written, and the restore binds them -- their
# type is registered at construction, before the queue is read.
cat > "$TEST_TMPDIR/typed.script" << EOS
scheduler.run 3000000
machine.adb.keyboard.type "abcdefgh"
scheduler.run 10000
assert checkpoint.save("$TEST_TMPDIR/typed.gs") "typed: save failed"
assert checkpoint.load("$TEST_TMPDIR/typed.gs") "typed: a checkpoint with typed keys pending did not restore"
scheduler.run 1000000
echo "TYPED ok"
quit
EOS
run se30 "$ROMS/iix-iicx-se30-97221136.rom" "$TEST_TMPDIR/typed.script" | grep -q '^TYPED ok'
echo "typed keys: restored"

echo "Failed-restore test passed"
