#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# compare-output.sh -- the integration corpus twice, with two emulator
# binaries, and a per-test diff of what each printed.  Stdout and stderr are
# captured to separate files, so a change that moves text between them is a
# difference too.  Used to show that a change which should not alter what a
# script prints (the structured-result annotations, say) does not.
#
#   scripts/compare-output.sh BIN_A BIN_B [TIER]      (default tier: unit)
#
# Tests listed in scripts/compare-output.expected (one test name per line, #
# comments) may differ: they print text the change is meant to change, such
# as `help` output.  Exit 0 when every other test matches, 1 otherwise.
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -ge 2 ] || { echo "usage: $0 BIN_A BIN_B [TIER]" >&2; exit 2; }
bin_a=$(readlink -f "$1")
bin_b=$(readlink -f "$2")
tier=${3:-unit}
out=build/compare-output
rm -rf "$out"
mkdir -p "$out/a" "$out/b"

# The wrapper the runner accepts (WRAPPER is a command prefix, followed by the
# runner's own binary path): run the chosen binary instead, appending its
# stdout and stderr to per-test files named after the test directory.
cat > "$out/wrapper.sh" <<'W'
#!/usr/bin/env bash
shift
name=$(basename "$PWD")
exec "$CMP_BIN" "$@" > >(tee -a "$CMP_DIR/$name.stdout") 2> >(tee -a "$CMP_DIR/$name.stderr" >&2)
W
chmod +x "$out/wrapper.sh"

# Lines that legitimately differ between two builds: the build stamp, the
# heartbeat, tmp paths and wall-clock durations (the same filters as
# compare-jobs-inline.sh), and the daemon's PID and port.
filter() {
    grep -v -E '^# running\.\.\.|^Granny Smith build|^gcc |^\[[0-9]+\] |^Daemon PID:' \
        | sed -E 's/ \(valgrind\)//; s#/tmp/[^ ]+#/tmp/X#g; s/[0-9]+(\.[0-9]+)? ?ms/N ms/g; s/(127\.0\.0\.1:|localhost )[0-9]+/\1PORT/g' || true
}

for side in a b; do
    bin=$bin_a
    [ "$side" = b ] && bin=$bin_b
    echo "== $side: $bin ($tier tier)"
    CMP_BIN="$bin" CMP_DIR="$PWD/$out/$side" \
        make -C tests/integration test TIER="$tier" -j"$(nproc)" WRAPPER="$PWD/$out/wrapper.sh" \
        > "$out/$side.log" 2>&1 || true
done

expected=$(grep -v -E '^\s*(#|$)' scripts/compare-output.expected 2>/dev/null || true)
fail=0
for f in "$out"/a/*.stdout "$out"/a/*.stderr "$out"/b/*.stdout "$out"/b/*.stderr; do
    [ -e "$f" ] || continue
    base=$(basename "$f")
    name=${base%.*}
    [ -n "$(grep -Fx "$name" <<<"$expected" || true)" ] && continue
    a="$out/a/$base"
    b="$out/b/$base"
    [ -f "$a.checked" ] && continue
    touch "$a.checked"
    if ! diff -u <(filter < "${a}" 2>/dev/null) <(filter < "${b}" 2>/dev/null) > "$out/$base.diff"; then
        echo "DIFFERENT: $base (see $out/$base.diff)"
        head -20 "$out/$base.diff"
        fail=1
    fi
done
[ "$fail" -eq 0 ] && echo "identical: every test not in compare-output.expected printed the same"
exit "$fail"
