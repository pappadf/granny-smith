#!/usr/bin/env bash
# bench.sh — wall-time harness for the Voodoo2 raster backends
# (proposal-voodoo2-raster-thread §8, shared with the walker-optimization
# proposal §3.8).
#
# Runs the canonical launch flow — the tnt-voodoo2-glide integration row:
# Mac OS 8.1 boots, Quake 3Dfx launches from the Finder, the attract
# demo plays to a fixed instruction count, the in-game golden is
# compared — once per requested backend, under `time`, and prints a
# table.  The instruction-count perf baselines cannot see host speed;
# this is the instrument that does.  NOT a CI gate (host-dependent): a
# tool for the before/after numbers in commit messages.
#
#   scripts/voodoo2/bench.sh [backend...]      default: sw thread null
#
# Each run is the real row (build if stale, media-gated, golden
# compared), so "PASS" beside a backend also says its frame was
# byte-identical to the walker's golden; `null` draws nothing and is
# EXPECTED to fail the golden — its time is the non-raster floor.
# Full output of every run lands in tmp/bench-<backend>.log.
#
# Reference rows (2-core devcontainer, release headless build):
#   2026-09-04  main 9c6f25c   sw 12m38s real / 11m53s user

set -u
cd "$(dirname "$0")/../.."

BACKENDS=("$@")
[ ${#BACKENDS[@]} -gt 0 ] || BACKENDS=(sw thread null)

mkdir -p tmp
# One emulator at a time: stray instances share the storage cache and
# steal the cores the timing is measuring.
for p in $(pgrep -x gs-headless); do kill -9 "$p"; done

TIMEFORMAT='%R %U'
printf '%-8s %-6s %10s %10s\n' backend result real user
for b in "${BACKENDS[@]}"; do
    log="tmp/bench-$b.log"
    # `time` reports the whole make (build + row); the build is a no-op
    # after the first backend.
    t=$( { time TEST_VARS="RASTER=$b" make -C tests/integration test-tnt-voodoo2-glide > "$log" 2>&1 ; } 2>&1 )
    if grep -q "=== PASS" "$log"; then res=PASS; else res=FAIL; fi
    real=${t% *}
    user=${t#* }
    printf '%-8s %-6s %9.1fs %9.1fs\n' "$b" "$res" "$real" "$user"
done
