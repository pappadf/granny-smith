#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# compare-jobs-inline.sh -- the integration corpus twice, threaded and with
# `--jobs=inline` (scripts on the emulator thread), and a diff of what the
# two printed.  The interpreter's thread must not change what a script
# does or says: the same output, the same verdicts.  The comparison is
# scripts/compare-integration-output.sh's.
#
#   scripts/compare-jobs-inline.sh [TIER]       (default: unit)
#
# Exit 0 when the runs match, 1 when they differ (the diffs are printed).
# Known, accepted difference: the daemon-statements probe "a client that
# disconnects mid-run cancels it" needs the driver's loop to poll the
# socket while the run is in flight, which the inline wait does not do.
set -euo pipefail
cd "$(dirname "$0")/.."
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

exec scripts/compare-integration-output.sh --tier "${1:-unit}" --out "$out" \
    "" "$PWD/$out/inline-wrapper.sh"
