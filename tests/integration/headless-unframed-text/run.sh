#!/usr/bin/env bash
# Feeds statements to unframed headless on stdin -- an echo and a failing
# assert, both carrying control bytes, a quote, a backslash and non-ASCII
# text, and a files.cp whose deferred (I/O worker) failure names a path with
# a quote and a backslash, and a files.xfer_read whose deferred answer (the
# bytes it read) only the work knows -- and checks stdout and stderr byte
# for byte (check.py).  Twice: with the job thread, and with --jobs=inline
# (scripts on the emulator thread, the I/O leaf working there and then),
# which must print the same bytes -- inline once lost the worker's reason to
# "files.cp: failed", and threaded printed the read as 0, the value the leaf
# returned before its work ran.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$WORK_DIR"
src="$WORK_DIR/src.txt"
printf 'x' > "$src"
in="$WORK_DIR/in.txt"
cat > "$in" <<'IN_EOF'
echo "o[\r][\x08][\x0c][\t][\"][\\][\x01][é😀]"
assert 1 == 2 "e[\r][\x08][\x0c][\t][\"][\\][\x01][é😀]"
IN_EOF
cat >> "$in" <<IN_EOF
echo "read=\${files.xfer_read("$src", 0, 8)}"
files.cp "$src" "/nonexistent-gs-dir/q\\"d\\\\e/out"
quit
IN_EOF
for mode in threaded inline; do
    flags=()
    [ "$mode" = inline ] && flags=(--jobs=inline)
    out="$WORK_DIR/out-$mode.txt"
    err="$WORK_DIR/err-$mode.txt"
    timeout -k 10 120 "$HEADLESS_BIN" rom="$ROM_PATH" --script-stdin --speed=turbo ${flags[@]+"${flags[@]}"} < "$in" > "$out" 2> "$err" || true
    python3 "$HERE/check.py" "$mode" "$out" "$err"
done
# And the two modes print the same bytes, not merely the expected lines.
for s in out err; do
    if ! cmp -s "$WORK_DIR/$s-threaded.txt" "$WORK_DIR/$s-inline.txt"; then
        echo "FAIL: std$s differs between threaded and --jobs=inline:"
        diff "$WORK_DIR/$s-threaded.txt" "$WORK_DIR/$s-inline.txt" || true
        exit 1
    fi
done
echo "headless-unframed-text: threaded and inline match"
