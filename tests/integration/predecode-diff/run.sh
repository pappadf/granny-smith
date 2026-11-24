#!/bin/bash
# Predecode differential runner (see config.mk): for each machine row, run
# the same script with predecode off, then on at each elision level, and
# compare every checkpoint pair with scripts/cmp-checkpoints.py.
#
# Environment from the runner: HEADLESS_BIN, TEST_DATA, TEST_RESULTS_DIR,
# STORAGE_CACHE, TEST_VAR_ARGS.  ROW=<name> restricts to one row.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CMP="$SCRIPT_DIR/../../../scripts/cmp-checkpoints.py"
cd "$TEST_RESULTS_DIR"

fail=0

# run <row> <config-name> <PD> <ELIDE> <rom> <script> [extra --var args]
# (JIT=1 in the environment: the translation tier on, translating eagerly)
run_one() {
    local row="$1" cfg="$2" pd="$3" elide="$4" rom="$5" script="$6"
    shift 6
    local out="$TEST_RESULTS_DIR/$row-$cfg"
    # --var values are strings; the switches are spliced in as literals.
    sed "s/\$PD\b/$pd/; s/\$ELIDE\b/$elide/; s/\$JIT\b/${JIT:-0}/" "$SCRIPT_DIR/$script" > "$out.lua"
    # shellcheck disable=SC2086 — TEST_VAR_ARGS is intentionally word-split
    GS_STORAGE_CACHE="$STORAGE_CACHE/$row-$cfg" "$HEADLESS_BIN" \
        rom="$rom" script="$out.lua" \
        --var ROM="$rom" --var OUT="$out" \
        --var TEST_DATA="$TEST_DATA" "$@" $TEST_VAR_ARGS --speed=turbo > "$out.log" 2>&1 \
        || { echo "  $row/$cfg: run failed (see $out.log)"; tail -5 "$out.log"; return 1; }
    grep -E "^cp|^stats" "$out.log" | sed "s/^/  $row\/$cfg: /"
    return 0
}

# compare <row> <config> <ncheckpoints>
compare() {
    local row="$1" cfg="$2" n="$3" i
    for i in $(seq 1 "$n"); do
        if python3 "$CMP" "$TEST_RESULTS_DIR/$row-off-$i.gs" "$TEST_RESULTS_DIR/$row-$cfg-$i.gs" > "$TEST_RESULTS_DIR/$row-$cfg-cmp$i.log" 2>&1; then
            echo "  $row: checkpoint $i off vs $cfg: identical guest state"
        else
            echo "  $row: checkpoint $i off vs $cfg: DIFFERS"
            cat "$TEST_RESULTS_DIR/$row-$cfg-cmp$i.log"
            fail=1
        fi
    done
}

# row <name> <rom> <script> <ncheckpoints> <elision levels...> -- extra vars
row() {
    local name="$1" rom="$2" script="$3" n="$4"
    shift 4
    local levels=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do levels+=("$1"); shift; done
    [ $# -gt 0 ] && shift
    if [ -n "${ROW:-}" ] && [ "$ROW" != "$name" ]; then
        echo "skip: $name (ROW=$ROW)"
        return
    fi
    echo "=== $name"
    run_one "$name" off 0 0 "$rom" "$script" "$@" || { fail=1; return; }
    for lv in "${levels[@]}"; do
        run_one "$name" "on$lv" 1 "$lv" "$rom" "$script" "$@" || { fail=1; continue; }
        compare "$name" "on$lv" "$n"
    done
    # The translation tier (a build that has it, on a row that names it):
    # the same checkpoints with hot code running as generated code
    if [ "$JIT_OK" = 1 ] && grep -q '\$JIT' "$SCRIPT_DIR/$script"; then
        JIT=1 run_one "$name" jit 1 0 "$rom" "$script" "$@" || { fail=1; return; }
        compare "$name" jit "$n"
    fi
}

# Whether this build can generate code (the wasm build run by Node)
JIT_OK=0
echo 'echo("@@JIT " .. gs.str(gs.try(function() return jit.available end, 0)))' > "$TEST_RESULTS_DIR/jit-probe.lua"
echo 'quit()' >> "$TEST_RESULTS_DIR/jit-probe.lua"
if "$HEADLESS_BIN" rom="$TEST_DATA/roms/plus-v3-4d1f8172.rom" script="$TEST_RESULTS_DIR/jit-probe.lua" 2>/dev/null |
    grep -q "@@JIT 1"; then
    JIT_OK=1
    echo "translation tier: available (each PowerPC row also runs with jit.enabled = 1)"
fi

FD_608="$TEST_DATA/systems/SSW-6.08-800K/SSW 6.0.8-800k Disk1of4"
HD_MARATHON="$TEST_DATA/apps/marathon_8_24gc.img"

row plus "$TEST_DATA/roms/plus-v3-4d1f8172.rom" plus.lua 3 0 1 2 -- --var FD="$FD_608"
row iicx "$TEST_DATA/roms/iix-iicx-se30-97221136.rom" iicx.lua 3 0 2 -- --var HD="$HD_MARATHON"
row pdm "$TEST_DATA/roms/pm6100-pm7100-pm8100-9feb69b3.rom" pdm.lua 3 0 --
row tnt "$TEST_DATA/roms/pm7500-pm8500-pm9500-96cd923d.rom" tnt.lua 3 0 --
row av "$TEST_DATA/roms/q840av-q660av-5bf10fd1.rom" av.lua 3 0 2 -- --var HD="$TEST_DATA/systems/system_7_1_77mb_av.img"

if [ "$fail" -ne 0 ]; then
    echo "predecode differential: FAILED"
    exit 1
fi
echo "predecode differential: all checkpoints identical"
