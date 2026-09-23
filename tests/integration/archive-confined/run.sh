#!/usr/bin/env bash
# archive.extract must not write outside its output directory (09 F-13).
set -euo pipefail

mkdir -p "$WORK_DIR"
ARCHIVE="$WORK_DIR/escape.sit"
OUT="$WORK_DIR/out"
rm -rf "$OUT" "$WORK_DIR/escape"
python3 "$(dirname "$0")/make_archive.py" "$ARCHIVE" "../escape" "payload"

SCRIPT="$WORK_DIR/extract.script"
printf 'archive.extract "%s" "%s"\nquit\n' "$ARCHIVE" "$OUT" > "$SCRIPT"
"$HEADLESS_BIN" rom="$ROM_PATH" script="$SCRIPT" --speed=max > "$WORK_DIR/extract.log" 2>&1 || true

fail=0
if [ -e "$WORK_DIR/escape" ]; then
    echo "FAIL: the entry escaped to $WORK_DIR/escape"
    fail=1
fi
if [ ! -f "$OUT/..:escape" ] || [ "$(cat "$OUT/..:escape")" != "payload" ]; then
    echo "FAIL: expected the entry inside the output directory as '..:escape'"
    ls -la "$OUT" 2>/dev/null || true
    fail=1
fi
if [ "$fail" -ne 0 ]; then
    echo "--- extract.log ---"
    cat "$WORK_DIR/extract.log"
    exit 1
fi
echo "extracted inside the output directory as '..:escape'; nothing escaped"
