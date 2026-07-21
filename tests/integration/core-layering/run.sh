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

# proposal-content-addressed-rom-provisioning.md §6: core may open a path it
# was handed, but must never FABRICATE one.  Two greps keep the boundary
# honest:
#   1. no environment path literal ("/opfs/…", "tests/data…") in the vROM
#      loader areas of src/core and src/machines — the platform enumerates
#      and offers files; core only content-matches among the offers.  (The
#      "/opfs/" *persistence* heuristics in src/core/storage et al. are a
#      separate is-this-path-durable concern, out of scope per §2.)
#   2. no catalog-name→path joining anywhere in src/ — vrom_catalog_name was
#      removed with the search; a reappearance means the name column leaked
#      back into core.
hits=$(grep -rnE '"(/opfs/|tests/data)' \
    "$ROOT/src/core/peripherals/nubus" "$ROOT/src/core/memory" "$ROOT/src/machines" 2>/dev/null || true)
if [ -n "$hits" ]; then
    echo "PATH-FABRICATION VIOLATION: environment path literal in a ROM/vROM loader area:"
    echo "$hits" | sed "s|$ROOT/||;s/^/    /"
    fail=1
fi
hits=$(grep -rn 'vrom_catalog_name' "$ROOT/src" 2>/dev/null || true)
if [ -n "$hits" ]; then
    echo "PATH-FABRICATION VIOLATION: vrom_catalog_name (catalog filename column) is back in src/:"
    echo "$hits" | sed "s|$ROOT/||;s/^/    /"
    fail=1
fi
if [ "$fail" -ne 0 ]; then
    echo "core-layering: FAILED — core must not fabricate ROM/vROM paths (proposal §6)"
    exit 1
fi
echo "core-layering: OK — no path fabrication in the ROM/vROM loader areas"
