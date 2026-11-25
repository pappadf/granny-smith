#!/usr/bin/env bash
# Feeds lines to unframed headless on stdin -- a print and a failing
# assert, both carrying control bytes, a quote, a backslash and non-ASCII
# text, and a files.cp whose deferred (I/O worker) failure names a path with
# a quote and a backslash, and a files.xfer_read whose deferred answer (the
# bytes it read) only the work knows -- and checks stdout and stderr byte
# for byte (check.py).  The job suspends while its I/O runs and resumes with
# the work's own answer or reason, never the provisional value the leaf
# returned before the work ran.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$WORK_DIR"
src="$WORK_DIR/src.txt"
printf 'x' > "$src"
in="$WORK_DIR/in.txt"
cat > "$in" <<'IN_EOF'
print("o[\r][\x08][\x0c][\t][\"][\\][\x01][é😀]")
assert(1 == 2, "e[\r][\x08][\x0c][\t][\"][\\][\x01][é😀]")
IN_EOF
cat >> "$in" <<IN_EOF
print("read=" .. files.xfer_read("$src", 0, 8))
files.cp "$src" "/nonexistent-gs-dir/q\\"d\\\\e/out"
quit
IN_EOF
out="$WORK_DIR/out.txt"
err="$WORK_DIR/err.txt"
timeout -k 10 120 "$HEADLESS_BIN" rom="$ROM_PATH" --script-stdin --speed=turbo < "$in" > "$out" 2> "$err" || true
python3 "$HERE/check.py" stdin "$out" "$err"
