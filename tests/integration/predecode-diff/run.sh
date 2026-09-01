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
run_one() {
    local row="$1" cfg="$2" pd="$3" elide="$4" rom="$5" script="$6"
    shift 6
    local out="$TEST_RESULTS_DIR/$row-$cfg"
    # --var values are strings; the two switches are spliced in as literals.
    sed "s/\$PD\b/$pd/; s/\$ELIDE\b/$elide/" "$SCRIPT_DIR/$script" > "$out.script"
    # shellcheck disable=SC2086 — TEST_VAR_ARGS is intentionally word-split
    GS_STORAGE_CACHE="$STORAGE_CACHE/$row-$cfg" "$HEADLESS_BIN" \
        rom="$rom" script="$out.script" \
        --var ROM="$rom" --var OUT="$out" \
        --var TEST_DATA="$TEST_DATA" "$@" $TEST_VAR_ARGS --speed=max > "$out.log" 2>&1 \
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
}

FD_608="$TEST_DATA/systems/SSW-6.08-800K/SSW 6.0.8-800k Disk1of4"
HD_MARATHON="$TEST_DATA/apps/marathon_8_24gc.img"

row plus "$TEST_DATA/roms/plus-v3-4d1f8172.rom" plus.script 3 0 1 2 -- --var FD="$FD_608"
row iicx "$TEST_DATA/roms/iix-iicx-se30-97221136.rom" iicx.script 3 0 2 -- --var HD="$HD_MARATHON"
row pdm "$TEST_DATA/roms/pm6100-pm7100-pm8100-9feb69b3.rom" pdm.script 3 0 --

if [ "$fail" -ne 0 ]; then
    echo "predecode differential: FAILED"
    exit 1
fi
echo "predecode differential: all checkpoints identical"
