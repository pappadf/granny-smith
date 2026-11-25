#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# compare-integration-output.sh -- the integration corpus twice, each run
# with the emulator behind its own WRAPPER (the command prefix
# tests/integration takes; "" for none), and a diff of what the two
# printed: per test, stdout and stderr apart (so text that moves between
# them is a difference too), and the tests' PASS / FAIL lines.  Lines two
# runs may legitimately print differently are filtered out
# (scripts/lib/compare-filter.sh).
#
#   scripts/compare-integration-output.sh [options] A_WRAPPER B_WRAPPER
#     --tier TIER     the tier to run (default: unit)
#     --tests "a b"   just these test directories (overrides --tier)
#     --allow NAME    a test that may differ (repeatable): one that
#                     exercises the change under comparison itself
#     --jobs N        parallel tests (default: nproc)
#     --out DIR       where the logs and diffs go (default:
#                     build/compare-integration-output)
#
# Exit 0 when every test not allowed to differ printed the same, 1 when one
# did not (the diffs are printed), 2 on a usage error.  Callers:
# scripts/compare-output.sh (two binaries).
set -euo pipefail
cd "$(dirname "$0")/.."

tier=unit
tests=
jobs=$(nproc)
out=build/compare-integration-output
allow=()
args=()
while [ $# -gt 0 ]; do
    case $1 in
        --tier) tier=$2; shift 2 ;;
        --tests) tests=$2; shift 2 ;;
        --allow) allow+=("$2"); shift 2 ;;
        --jobs) jobs=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        --) shift; args+=("$@"); break ;;
        -*) echo "$0: unknown option $1" >&2; exit 2 ;;
        *) args+=("$1"); shift ;;
    esac
done
[ ${#args[@]} -eq 2 ] || { echo "usage: $0 [options] A_WRAPPER B_WRAPPER" >&2; exit 2; }
out=$(mkdir -p "$out" && cd "$out" && pwd)
rm -rf "$out/a" "$out/b" "$out"/*.diff
mkdir -p "$out/a" "$out/b"

# The WRAPPER each run hands the runner: the side's own wrapper (word-split,
# as the runner splits WRAPPER) in front of the real binary and its
# arguments, with its stdout and stderr appended to per-test files named
# after the test directory (the runner starts the emulator in it).
cat > "$out/capture.sh" <<'W'
#!/usr/bin/env bash
name=$(basename "$PWD")
# shellcheck disable=SC2086 -- CMP_INNER is a command prefix
exec $CMP_INNER "$@" > >(tee -a "$CMP_DIR/$name.stdout") 2> >(tee -a "$CMP_DIR/$name.stderr" >&2)
W
chmod +x "$out/capture.sh"

# Lines that legitimately differ between two runs (scripts/lib).
. scripts/lib/compare-filter.sh

select=(TIER="$tier")
[ -n "$tests" ] && select=(TESTS="$tests")
for side in a b; do
    inner=${args[0]}
    [ "$side" = b ] && inner=${args[1]}
    echo "== $side: ${inner:-(no wrapper)} (${tests:-$tier tier})"
    CMP_INNER="$inner" CMP_DIR="$out/$side" \
        make -C tests/integration test "${select[@]}" -j"$jobs" WRAPPER="$out/capture.sh" \
        > "$out/$side.log" 2>&1 || true
done

allowed() {
    local a
    for a in "${allow[@]+"${allow[@]}"}"; do [ "$a" = "$1" ] && return 0; done
    return 1
}

fail=0
# Each test's verdict, in a stable order (parallel runs interleave).
verdicts() { grep -E '(PASS|FAIL|SKIP)' "$1" | compare_filter | sort; }
if ! diff -u <(verdicts "$out/a.log") <(verdicts "$out/b.log") > "$out/verdicts.diff"; then
    echo "DIFFERENT: test verdicts (see $out/verdicts.diff)"
    head -20 "$out/verdicts.diff"
    fail=1
fi
for f in "$out"/a/* "$out"/b/*; do
    [ -e "$f" ] || continue
    base=$(basename "$f")
    allowed "${base%.*}" && continue
    [ -f "$out/$base.diff" ] && continue
    if ! diff -u <(compare_filter < "$out/a/$base" 2>/dev/null) \
                 <(compare_filter < "$out/b/$base" 2>/dev/null) > "$out/$base.diff"; then
        echo "DIFFERENT: $base (see $out/$base.diff)"
        head -20 "$out/$base.diff"
        fail=1
    fi
done
[ "$fail" -eq 0 ] && echo "identical: $(ls "$out/a" | grep -c '\.stdout$') tests printed the same both ways"
exit "$fail"
