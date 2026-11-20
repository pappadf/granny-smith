#!/usr/bin/env bash
# Feeds two statements to unframed headless on stdin -- an echo and a
# failing assert, both carrying control bytes, a quote, a backslash and
# non-ASCII text -- and checks stdout and stderr byte for byte (check.py).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$WORK_DIR"
in="$WORK_DIR/in.txt"
cat > "$in" <<'IN_EOF'
echo "o[\r][\x08][\x0c][\t][\"][\\][\x01][é😀]"
assert 1 == 2 "e[\r][\x08][\x0c][\t][\"][\\][\x01][é😀]"
quit
IN_EOF
out="$WORK_DIR/out.txt"
err="$WORK_DIR/err.txt"
timeout -k 10 120 "$HEADLESS_BIN" rom="$ROM_PATH" --script-stdin --speed=turbo < "$in" > "$out" 2> "$err" || true
python3 "$HERE/check.py" "$out" "$err"
