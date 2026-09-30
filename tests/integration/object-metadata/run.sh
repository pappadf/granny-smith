#!/usr/bin/env bash
# Runs test.script, then compares each usage block it printed with its
# golden file (usage-<path>.txt).  REGEN=1 in TEST_VARS rewrites them.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$WORK_DIR"
out="$WORK_DIR/out.log"
# shellcheck disable=SC2086
"$HEADLESS_BIN" rom="$ROM_PATH" model=iicx script=test.script --speed=turbo $TEST_VAR_ARGS > "$out" 2>&1 || {
    cat "$out"
    exit 1
}
cat "$out"
grep -q '^object-metadata-ok$' "$out" || exit 1
regen=0
case " $TEST_VAR_ARGS " in *" REGEN=1 "*) regen=1 ;; esac
fail=0
for path in debug.breakpoints.add 'machine.floppy.drive[0].insert' scheduler.mode machine.cpu.d0 machine.floppy; do
    golden="$HERE/usage-$(echo "$path" | tr -c 'A-Za-z0-9._\n' '_').txt"
    got="$WORK_DIR/$(basename "$golden")"
    awk -v p="=== USAGE $path" '$0 == p {on = 1; next} /^=== (USAGE|END)/ {on = 0} on' "$out" > "$got"
    if [ "$regen" = 1 ]; then
        cp "$got" "$golden"
    elif ! diff -u "$golden" "$got"; then
        echo "FAIL: usage text of $path differs from $(basename "$golden")"
        fail=1
    fi
done
exit "$fail"
