#!/usr/bin/env bash
# catalog.profile() schema-snapshot test.
#
# Dumps catalog.profile for every registered model, normalizes each profile to
# a value-independent SHAPE string (see schema.mjs), and diffs against the
# committed golden snapshot. Fails loudly when a field is added, removed, or
# retyped — the JSON the frontend (and any other consumer) probes is a
# contract, and silent shape drift is exactly what broke web-legacy once.
set -euo pipefail

OUT="$WORK_DIR/profiles.txt"
SCRIPT="$WORK_DIR/profiles.script"
ACTUAL="$WORK_DIR/schema.actual"
mkdir -p "$WORK_DIR"

# Every registered model, read from the emulator itself (catalog.models),
# so a newly registered machine cannot be silently left out.
cat > "$SCRIPT" <<'SCRIPT'
let ms = catalog.models
for m in $ms {
    echo "${catalog.profile($m)}"
}
quit
SCRIPT

"$HEADLESS_BIN" rom="$ROM_PATH" script="$SCRIPT" --speed=turbo > "$OUT" 2>&1

node ./schema.mjs < "$OUT" > "$ACTUAL"

if ! diff -u ./schema.expected "$ACTUAL"; then
    echo ""
    echo "FAIL: catalog.profile() schema drift (diff above: - golden, + actual)."
    echo "If the shape change is intentional, regenerate the golden:"
    echo "  make -C tests/integration test-machine-profile-schema  # then copy build/integration/.../schema.actual"
    echo "  cp build/integration/machine-profile-schema/schema.actual \\"
    echo "     tests/integration/machine-profile-schema/schema.expected"
    echo ""
    echo "--- captured profile output ---"
    cat "$OUT"
    exit 1
fi

echo "machine-profile-schema: all $(wc -l < ./schema.expected | tr -d ' ') model schemas match the golden snapshot"
