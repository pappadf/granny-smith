#!/usr/bin/env bash
# Feeds jobs to headless --framed on stdin and checks the record stream
# (check.py): annotation order, value text between its markers, an error
# written once (as its record, not also to stderr), the reduced form of an
# oversized value, and escaped control-byte output that fits the ring.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$WORK_DIR"
ctl="$WORK_DIR/ctl.bin"
python3 -c "import sys; sys.stdout.buffer.write(bytes([1]) * 65536)" > "$ctl"
in="$WORK_DIR/in.txt"
cat > "$in" <<IN_EOF
if true {
    echo "before"
    machine.cpu.pc
    echo "after"
}
assert 1 == 2 "boom"
let s = "x"
while len(\$s) < 200000 { \$s = "\$s\$s" }
\$s
files.cat "$ctl"
echo "done"
quit
IN_EOF
out="$WORK_DIR/out.txt"
err="$WORK_DIR/err.txt"
timeout -k 10 120 "$HEADLESS_BIN" rom="$ROM_PATH" model=iicx --framed --script-stdin --speed=turbo < "$in" > "$out" 2> "$err" || true
python3 "$HERE/check.py" "$out" "$err"
