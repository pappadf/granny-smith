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
