#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# compare-jobs-inline.sh -- the integration corpus twice, threaded and with
# `--jobs=inline` (scripts on the emulator thread), and a diff of what the
# two printed.  The interpreter's thread must not change what a script
# does or says: the same stdout, the same exit codes.  Wall-clock lines
# (the once-a-second `# running...` heartbeat) are filtered out.
#
#   scripts/compare-jobs-inline.sh [TIER]       (default: unit)
#
# Exit 0 when the runs match, 1 when they differ (the diff is printed).
set -euo pipefail
cd "$(dirname "$0")/.."
tier=${1:-unit}
out=build/compare-jobs-inline
mkdir -p "$out"

# A wrapper the runner accepts (WRAPPER is a command prefix): the real
# binary with the flag appended.
cat > "$out/inline-wrapper.sh" <<'W'
#!/usr/bin/env bash
bin=$1; shift
exec "$bin" --jobs=inline "$@"
W
chmod +x "$out/inline-wrapper.sh"

# Lines that legitimately differ: the heartbeat, the build stamp (the
# binary relinks between runs), the runner's wrapper label, tmp paths,
# compile lines from a rebuild, and wall-clock durations.
filter() {
    grep -v -E '^# running\.\.\.|^Granny Smith build|^gcc |^\[[0-9]+\] ' \
        | sed -E 's/ \(valgrind\)//; s#/tmp/[^ ]+#/tmp/X#g; s/[0-9]+(\.[0-9]+)? ?ms/N ms/g'
}

echo "== threaded ($tier tier)"
make -C tests/integration test TIER="$tier" -j1 2>&1 | filter > "$out/threaded.log" || true
echo "== inline ($tier tier)"
make -C tests/integration test TIER="$tier" -j1 WRAPPER="$PWD/$out/inline-wrapper.sh" 2>&1 | filter > "$out/inline.log" || true

if diff -u "$out/threaded.log" "$out/inline.log" > "$out/diff.txt"; then
    echo "identical: $(grep -c 'PASS' "$out/threaded.log") passes both ways"
    exit 0
fi
# Known, accepted difference: the daemon-statements probe "a client that
# disconnects mid-run cancels it" needs the driver's loop to poll the
# socket while the run is in flight, which the inline wait does not do.
echo "DIFFERENT (see $out/diff.txt):"
head -80 "$out/diff.txt"
exit 1
