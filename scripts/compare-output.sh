#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# compare-output.sh -- the integration corpus twice, with two emulator
# binaries, and a per-test diff of what each printed (stdout and stderr
# apart).  Used to show that a change which should not alter what a script
# prints (the structured-result annotations, say) does not.  The
# comparison is scripts/compare-integration-output.sh's.
#
#   scripts/compare-output.sh BIN_A BIN_B [TIER] [--allow NAME]...
#
# --allow names a test that may differ: one that prints text the change is
# meant to change, such as `help` output.  Exit 0 when every other test
# matches, 1 otherwise.
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -ge 2 ] || { echo "usage: $0 BIN_A BIN_B [TIER] [--allow NAME]..." >&2; exit 2; }
bin_a=$(readlink -f "$1")
bin_b=$(readlink -f "$2")
shift 2
tier=unit
if [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; then tier=$1; shift; fi
out=build/compare-output
mkdir -p "$out"

# A wrapper per side (WRAPPER is a command prefix, followed by the runner's
# own binary path): run the chosen binary instead.
for side in a b; do
    bin=$bin_a
    [ "$side" = b ] && bin=$bin_b
    printf '#!/usr/bin/env bash\nshift\nexec %q "$@"\n' "$bin" > "$out/bin-$side.sh"
    chmod +x "$out/bin-$side.sh"
done

exec scripts/compare-integration-output.sh --tier "$tier" --out "$out" "$@" \
    "$PWD/$out/bin-a.sh" "$PWD/$out/bin-b.sh"
