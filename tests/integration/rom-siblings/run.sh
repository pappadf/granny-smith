#!/usr/bin/env bash
# Env supplied by the runner: HEADLESS_BIN, TEST_DATA, TEST_RESULTS_DIR, WORK_DIR.
#
# The CLI ROM sits alone in cli/, so the startup walk offers nothing.  The
# IIcx ROM and the 8•24 GC's declaration ROM sit together in cards/; the
# script boots from there and picks the card, which resolves only if the
# boot offered its siblings.
set -euo pipefail
mkdir -p "$WORK_DIR/cli" "$WORK_DIR/cards" "$TEST_RESULTS_DIR"
cp "$TEST_DATA/roms/plus-v3-4d1f8172.rom" "$WORK_DIR/cli/plus.rom"
cp "$TEST_DATA/roms/iix-iicx-se30-97221136.rom" "$WORK_DIR/cards/iicx.rom"
cp "$TEST_DATA/roms/824gc-v1.1-revb-d722b053.vrom" "$WORK_DIR/cards/"

cat > "$WORK_DIR/boot.script" <<SCRIPT
machine.boot model="iicx" ram=8192 video_card="824gc" rom="$WORK_DIR/cards/iicx.rom"
assert machine.nubus.slot[9].card.name == "Apple Macintosh Display Card 8•24 GC" "slot \$9 card != 8•24 GC"
assert machine.nubus.slot[9].card.declrom.present "declaration ROM beside the booted ROM was not offered"
echo "rom-siblings-ok"
quit
SCRIPT

OUT="$TEST_RESULTS_DIR/out.log"
"$HEADLESS_BIN" "rom=$WORK_DIR/cli/plus.rom" "script=$WORK_DIR/boot.script" --speed=max > "$OUT" 2>&1 || true
if ! grep -q "rom-siblings-ok" "$OUT"; then
    cat "$OUT"
    echo "FAIL: the card ROM beside the script-booted ROM was not offered"
    exit 1
fi
echo "rom-siblings: passed"
