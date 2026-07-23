#!/usr/bin/env bash
# machine.profile() schema-snapshot test (proposal §6.1).
#
# Dumps machine.profile for every registered model, normalizes each profile to
# a value-independent SHAPE string (see schema.mjs), and diffs against the
# committed golden snapshot. Fails loudly when a field is added, removed, or
# retyped — the JSON the frontend (and any other consumer) probes is a
# contract, and silent shape drift is exactly what broke web-legacy once.
set -euo pipefail

OUT="$WORK_DIR/profiles.txt"
SCRIPT="$WORK_DIR/profiles.script"
ACTUAL="$WORK_DIR/schema.actual"
mkdir -p "$WORK_DIR"

MODELS="plus se30 iicx iix iifx iici iisi lisa macxl"

: > "$SCRIPT"
for m in $MODELS; do
    echo "echo \"\${machine.profile(\"$m\")}\"" >> "$SCRIPT"
done
echo "quit" >> "$SCRIPT"

"$HEADLESS_BIN" rom="$ROM_PATH" script="$SCRIPT" --speed=max > "$OUT" 2>&1

node ./schema.mjs < "$OUT" > "$ACTUAL"

if ! diff -u ./schema.expected "$ACTUAL"; then
    echo ""
    echo "FAIL: machine.profile() schema drift (diff above: - golden, + actual)."
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
