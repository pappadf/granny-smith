#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# bench-image-boot.sh - boot a machine from a raw disk image and from the same
# disk stored as UDIF (.dmg), and compare.
#
# For each row: convert the raw image to UDIF once (files.convert), then boot
# the same machine for the same number of instructions from each, RUNS times,
# in fresh headless processes at turbo speed.  Reports the median wall-clock
# time of each, the UDIF overhead, the disk-image cache's counters, and
# whether the two boots end on identical screens (they must: the emulator is
# deterministic and the disks decode to the same bytes).
#
# Usage:
#   scripts/bench-image-boot.sh [ROWS...]       # default: every row
#   RUNS=5 CACHE_MB=16 scripts/bench-image-boot.sh sys76
#
# Rows (test data from scripts/fetch-test-data.sh):
#   sys76   Quadra 700, System 7.6 (170 MB)
#   sys75   IIci, System 7.5 (77 MB)
#   aux     IIx, A/UX 3.0.1 (160 MB)
#
# Environment: RUNS (default 5), CACHE_MB (files.cache.image_mb, default
# the core's), HEADLESS (default build/headless/gs-headless), OUT (work
# directory, default tmp/bench-image-boot).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA="$ROOT/tests/data"
HEADLESS="${HEADLESS:-$ROOT/build/headless/gs-headless}"
RUNS="${RUNS:-5}"
OUT="${OUT:-$ROOT/tmp/bench-image-boot}"
CACHE_MB="${CACHE_MB:-}"

# name | model | ram KB | rom | disk | instructions
ROWS_ALL=(
    "sys76|q700|8192|roms/q700-q900-420dbff3.rom|systems/system_7_6_170mb_24ac.img|900000000"
    "sys75|iici|8192|roms/iici-368cadfe.rom|systems/system_7_5_0_77mb_mode32_24ac.img|260000000"
    "aux|iix|16384|roms/iix-iicx-se30-97221136.rom|aux/aux_3.0.1/hd160-with-aux-301.img|900000000"
)

[ -x "$HEADLESS" ] || { echo "no headless binary at $HEADLESS (make headless)" >&2; exit 1; }
mkdir -p "$OUT"

want=("$@")
selected() {
    [ ${#want[@]} -eq 0 ] && return 0
    local w
    for w in "${want[@]}"; do [ "$w" = "$1" ] && return 0; done
    return 1
}

now() { date +%s.%N; }

median() { sort -n | awk '{a[NR]=$1} END {print (NR%2) ? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2}'; }

# Run a headless script in a fresh process, quietly; its output to $2.
run_script() {
    local rom="$1" script="$2" log="$3"
    GS_STORAGE_CACHE="$OUT/cache" "$HEADLESS" rom="$rom" script="$script" --speed=turbo >"$log" 2>&1
}

printf '%-6s %-5s %10s %10s %9s %12s %12s  %s\n' row model raw_s udif_s overhead udif_misses udif_hits screens
for spec in "${ROWS_ALL[@]}"; do
    IFS='|' read -r name model ram rom disk instr <<<"$spec"
    selected "$name" || continue
    raw="$DATA/$disk"
    romp="$DATA/$rom"
    if [ ! -f "$raw" ] || [ ! -f "$romp" ]; then
        printf '%-6s skipped: no %s or %s\n' "$name" "$disk" "$rom"
        continue
    fi
    dmg="$OUT/$name.dmg"
    if [ ! -f "$dmg" ]; then
        cat >"$OUT/$name-convert.script" <<EOF
assert files.convert("$raw", "$dmg") "convert failed"
quit
EOF
        run_script "$romp" "$OUT/$name-convert.script" "$OUT/$name-convert.log"
    fi

    for kind in raw udif; do
        img="$raw"
        [ "$kind" = udif ] && img="$dmg"
        # The boot copies nothing: the base is opened read-only and the
        # writes go to a delta in the cache directory.
        cat >"$OUT/$name-$kind.script" <<EOF
${CACHE_MB:+files.cache.image_mb = $CACHE_MB}
machine.boot model="$model" ram=$ram rom="$romp"
machine.rtc.time = 3000000000
machine.scsi.attach_hd "$img" 0
scheduler.run $instr
machine.screen.save "$OUT/$name-$kind.png"
echo "image_misses=\${files.cache.image_misses} image_hits=\${files.cache.image_hits}"
quit
EOF
        : >"$OUT/$name-$kind.times"
        for _ in $(seq "$RUNS"); do
            rm -rf "$OUT/cache"
            t0=$(now)
            run_script "$romp" "$OUT/$name-$kind.script" "$OUT/$name-$kind.log"
            t1=$(now)
            echo "$t1 - $t0" | bc >>"$OUT/$name-$kind.times"
        done
    done
    raw_s=$(median <"$OUT/$name-raw.times")
    udif_s=$(median <"$OUT/$name-udif.times")
    over=$(echo "scale=1; 100 * ($udif_s - $raw_s) / $raw_s" | bc)
    stats=$(grep -o 'image_misses=[0-9]* image_hits=[0-9]*' "$OUT/$name-udif.log" | tail -1)
    misses=${stats#image_misses=}
    misses=${misses%% *}
    hits=${stats##*image_hits=}
    same=differ
    cmp -s "$OUT/$name-raw.png" "$OUT/$name-udif.png" && same=identical
    printf '%-6s %-5s %10.2f %10.2f %8s%% %12s %12s  %s\n' "$name" "$model" "$raw_s" "$udif_s" "$over" "$misses" "$hits" "$same"
done
