#!/usr/bin/env bash
# Documentation completeness of the object model.
#
# shell.lint_members() walks the live tree and reports each documentation
# gap as `<path>: <rule>` (src/core/object/lint.c lists the rules):
# an argument or node with no doc, an untyped argument, a VK_ANY result with
# nothing said about it, an enum without its values, a default mentioned in
# prose but not declared.  help, the command browser and the argument forms
# show exactly these, so a gap is something a user sees as blank or wrong.
#
# Booting is what makes the walk total: a device's members exist only on the
# machine that has it.  The same per-model boot list as object-class-registry.
#
# allow.txt holds known gaps.  It is empty, and may only shrink: a line
# reported here that is not in it fails the test, and so does a line in it
# that no model reports any more (remove it).  Regenerate with REGEN=1.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ALLOW="$HERE/allow.txt"
SCRIPT="$WORK_DIR/lint.script"
mkdir -p "$WORK_DIR"
cat > "$SCRIPT" <<'SCRIPT_EOF'
scheduler.run 200000
for l in shell.lint_members() {
    echo "LINT $l"
}
quit
SCRIPT_EOF

# model:rom-under-tests/data/roms
MODELS="
plus:plus-v3-4d1f8172.rom
se30:iix-iicx-se30-97221136.rom
iix:iix-iicx-se30-97221136.rom
iicx:iix-iicx-se30-97221136.rom
iifx:iifx-4147dd77.rom
iici:iici-368cadfe.rom
iisi:iisi-36b7fb6c.rom
q700:q700-q900-420dbff3.rom
q900:q700-q900-420dbff3.rom
q950:q950-3dc27823.rom
q840av:q840av-q660av-5bf10fd1.rom
q660av:q840av-q660av-5bf10fd1.rom
pm6100:pm6100-pm7100-pm8100-9feb69b3.rom
pm7100:pm6100-pm7100-pm8100-9feb69b3.rom
pm8100:pm6100-pm7100-pm8100-9feb69b3.rom
pm7500:pm7500-pm8500-pm9500-96cd923d.rom
pm8500:pm7500-pm8500-pm9500-96cd923d.rom
pm9500:pm7500-pm8500-pm9500-96cd923d.rom
ans500:ans500-ans700-962f6c13.rom
ans700:ans500-ans700-962f6c13.rom
lisa:lisa2-revh-098917b2.rom
macxl:macxl-3a-094c82f0.rom
"

found="$WORK_DIR/found.txt"
: > "$found"
fail=0
for entry in $MODELS; do
    model="${entry%%:*}"
    rom="$TEST_DATA/roms/${entry##*:}"
    if [ ! -f "$rom" ]; then
        echo "FAIL: $model: ROM not found: $rom"
        fail=1
        continue
    fi
    out="$WORK_DIR/$model.log"
    "$HEADLESS_BIN" rom="$rom" model="$model" script="$SCRIPT" --speed=turbo > "$out" 2>&1 || true
    sed -n 's/^LINT //p' "$out" >> "$found"
done
LC_ALL=C sort -u "$found" -o "$found"

if [ "${REGEN:-0}" = "1" ]; then
    cp "$found" "$ALLOW"
    echo "member-docs: allow.txt regenerated ($(wc -l < "$ALLOW") lines)"
    exit 0
fi

new=$(LC_ALL=C comm -23 "$found" <(LC_ALL=C sort -u "$ALLOW") || true)
stale=$(LC_ALL=C comm -13 "$found" <(LC_ALL=C sort -u "$ALLOW") || true)
if [ -n "$new" ]; then
    echo "FAIL: documentation gaps not in allow.txt (document them; do not add them):"
    echo "$new" | sed 's/^/    /'
    fail=1
fi
if [ -n "$stale" ]; then
    echo "FAIL: allow.txt lists gaps no model reports any more (remove them):"
    echo "$stale" | sed 's/^/    /'
    fail=1
fi
[ "$fail" -eq 0 ] || exit 1
echo "member-docs: $(wc -l < "$found") known gap(s), none new"
