#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
# files.archive.import must step into an archive held in a wrapper's
# resource fork.
set -euo pipefail

mkdir -p "$WORK_DIR"
BIN="$WORK_DIR/Installer.sea.bin"
OUT="$WORK_DIR/imported.dmg"
rm -f "$OUT"
python3 "$(dirname "$0")/make_sea_bin.py" "$BIN" "Disk.image"

SCRIPT="$WORK_DIR/import.script"
cat > "$SCRIPT" <<EOS
assert files.archive.import("$BIN", "$OUT") "archive import failed"
let mi = files.udif_info("$OUT")
echo "source_name=\${\$mi.source_name}"
echo "bytes=\${\$mi.bytes}"
quit
EOS
"$HEADLESS_BIN" rom="$ROM_PATH" script="$SCRIPT" --speed=turbo > "$WORK_DIR/import.log" 2>&1 || true

if ! grep -q '^source_name=Disk.image$' "$WORK_DIR/import.log" || ! grep -q '^bytes=4096$' "$WORK_DIR/import.log"; then
    echo "FAIL: expected the StuffIt member Disk.image (4096 bytes) to be imported"
    echo "--- import.log ---"
    cat "$WORK_DIR/import.log"
    exit 1
fi
echo "imported Disk.image from the archive in the resource fork"
