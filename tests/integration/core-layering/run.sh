#!/usr/bin/env bash
# Enforce the proposal §4.3 layering rule: no file under src/core/ may #include
# a machine *implementation* header (anything under src/machines/).  The one
# legal machine header for core is core/machine_profile.h, which lives in core
# — so it never appears in the src/machines/ header set this check builds.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
fail=0

# Every header basename that lives under src/machines/ is off-limits to core.
while IFS= read -r hdr; do
    base=$(basename "$hdr")
    # Find core files that #include this machine header by basename.
    hits=$(grep -rln "#include \"$base\"" "$ROOT/src/core/" 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "LAYERING VIOLATION: src/core/ includes machine header '$base':"
        echo "$hits" | sed "s|$ROOT/||;s/^/    /"
        fail=1
    fi
done < <(find "$ROOT/src/machines" -name '*.h')

if [ "$fail" -ne 0 ]; then
    echo "core-layering: FAILED — core must include only core/machine_profile.h"
    exit 1
fi
echo "core-layering: OK — src/core/ includes no machine-implementation headers"
