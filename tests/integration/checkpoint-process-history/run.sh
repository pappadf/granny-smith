#!/bin/bash
# Checkpoint process-history test (see config.mk).
#
#   Step 1  boot a IIcx (its GLUE I/O charges wait-state penalties on every
#           VIA access), save a checkpoint mid-run, run on and print the
#           machine's state.
#   Step 2  a fresh process restores the checkpoint, runs as far and prints.
#   Step 3  a process that ran a Quadra 700 first (its own I/O penalties)
#           restores the same checkpoint, runs as far and
#           prints.
# All three states must be identical: nothing a restore depends on may come
# from what the process did before.

set -e

# Set by the parent Makefile: HEADLESS_BIN, ROM_PATH, TEST_DATA, TEST_TMPDIR,
# STORAGE_CACHE, TEST_VAR_ARGS.

CHECKPOINT_FILE="$TEST_TMPDIR/history.gs"
Q700_ROM="$TEST_DATA/roms/q700-q900-420dbff3.rom"

# The state a run is compared by: CPU registers and the scheduler's clock.
STATE='echo "STATE pc=${machine.cpu.pc} d0=${machine.cpu.d0} d1=${machine.cpu.d1} a0=${machine.cpu.a0} a7=${machine.cpu.a7} sr=${machine.cpu.sr} cycles=${scheduler.cycles} instr=${scheduler.instr_count}"'

run() {
    # shellcheck disable=SC2086 — TEST_VAR_ARGS is intentionally word-split
    GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
        rom="$ROM_PATH" model=iicx \
        script="$1" \
        $TEST_VAR_ARGS \
        --speed=turbo
}

cat > "$TEST_TMPDIR/step1.script" << EOF
scheduler.run 30000000
assert checkpoint.save("$CHECKPOINT_FILE") "save failed"
scheduler.run 5000000
$STATE
quit
EOF

cat > "$TEST_TMPDIR/step2.script" << EOF
assert checkpoint.load("$CHECKPOINT_FILE") "load failed"
scheduler.run 5000000
$STATE
quit
EOF

cat > "$TEST_TMPDIR/step3.script" << EOF
machine.boot model="q700" rom="$Q700_ROM"
scheduler.run 20000000
assert checkpoint.load("$CHECKPOINT_FILE") "load failed"
scheduler.run 5000000
$STATE
quit
EOF

echo "Step 1: run, save, run on"
s1=$(run "$TEST_TMPDIR/step1.script" | grep '^STATE')
echo "$s1"
echo "Step 2: restore in a fresh process"
s2=$(run "$TEST_TMPDIR/step2.script" | grep '^STATE')
echo "$s2"
echo "Step 3: restore in a process that ran a Quadra 700 first"
s3=$(run "$TEST_TMPDIR/step3.script" | grep '^STATE')
echo "$s3"

if [ -z "$s1" ] || [ "$s1" != "$s2" ] || [ "$s1" != "$s3" ]; then
    echo "ERROR: the restored runs differ from the original"
    exit 1
fi
rm -f "$CHECKPOINT_FILE"
echo "Checkpoint process-history test passed"
