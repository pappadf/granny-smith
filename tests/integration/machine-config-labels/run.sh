#!/usr/bin/env bash
# Dump catalog.profile for every registered model and lint its labels
# (lint.mjs).
set -euo pipefail

OUT="$WORK_DIR/profiles.txt"
SCRIPT="$WORK_DIR/profiles.lua"
mkdir -p "$WORK_DIR"

cat > "$SCRIPT" <<'SCRIPT'
local ms = catalog.models
for _, m in ipairs(ms) do
    echo(F"${catalog.profile(m)}")
end
quit()
SCRIPT

"$HEADLESS_BIN" rom="$ROM_PATH" script="$SCRIPT" --speed=turbo > "$OUT" 2>&1
node ./lint.mjs < "$OUT"
