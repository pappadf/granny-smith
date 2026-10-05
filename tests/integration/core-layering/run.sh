#!/usr/bin/env bash
# Enforce the core layering rule: no file under src/core/ may #include
# a machine *implementation* header (anything under src/machines/).  The one
# legal machine header for core is core/machine_profile.h, which lives in core
# — so it never appears in the src/machines/ header set this check builds.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
fail=0

# Every header basename that lives under src/machines/ is off-limits to core.
while IFS= read -r hdr; do
    base=$(basename "$hdr")
    # Find core files that #include this machine header by basename.
    hits=$(grep -rln "#include \"$base\"" "$ROOT/src/core/" 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "LAYERING VIOLATION: src/core/ includes machine header '$base':"
        echo "$hits" | sed "s|$ROOT/||;s/^/    /"
        fail=1
    fi
done < <(find "$ROOT/src/machines" -name '*.h')

if [ "$fail" -ne 0 ]; then
    echo "core-layering: FAILED — core must include only core/machine_profile.h"
    exit 1
fi
echo "core-layering: OK — src/core/ includes no machine-implementation headers"

# Core may open a path it was handed, but must never FABRICATE one.  Two
# greps keep the boundary honest:
#   1. no environment path literal ("/opfs/…", "tests/data…") in the vROM
#      loader areas of src/core and src/machines — the platform enumerates
#      and offers files; core only content-matches among the offers.  (The
#      "/opfs/" *persistence* heuristics in src/core/storage et al. are a
#      separate is-this-path-durable concern, out of scope here.)
#   2. no catalog-name→path joining anywhere in src/ — vrom_catalog_name was
#      removed with the search; a reappearance means the name column leaked
#      back into core.
hits=$(grep -rnE '"(/opfs/|tests/data)' \
    "$ROOT/src/core/peripherals/nubus" "$ROOT/src/core/memory" "$ROOT/src/machines" 2>/dev/null || true)
if [ -n "$hits" ]; then
    echo "PATH-FABRICATION VIOLATION: environment path literal in a ROM/vROM loader area:"
    echo "$hits" | sed "s|$ROOT/||;s/^/    /"
    fail=1
fi
hits=$(grep -rn 'vrom_catalog_name' "$ROOT/src" 2>/dev/null || true)
if [ -n "$hits" ]; then
    echo "PATH-FABRICATION VIOLATION: vrom_catalog_name (catalog filename column) is back in src/:"
    echo "$hits" | sed "s|$ROOT/||;s/^/    /"
    fail=1
fi
if [ "$fail" -ne 0 ]; then
    echo "core-layering: FAILED — core must not fabricate ROM/vROM paths"
    exit 1
fi
echo "core-layering: OK — no path fabrication in the ROM/vROM loader areas"

# A class_desc_t is file-local unless another file uses it.
#
# 87 of 127 were non-static, and only 7 had a consumer outside their own file.
# The other 80 exported a global symbol for nothing -- and, worse, there was no
# way to tell from a declaration whether a class was part of another file's
# contract.  Five of them were declared `extern` in their own headers while
# nothing imported them, claiming a contract that did not exist.
#
# via_port_a_class and via_port_b_class are the live hazard this prevents:
# both were non-static and both named "via_port", a link-time collision
# waiting for either to move.
#
# The allow-list is the set with a real cross-file consumer.  Adding to it is
# a deliberate act; growing it by accident is what this check stops.
ALLOWED_EXTERNAL_CLASSES="
display_fb_class
find_class
shell_alias_class
shell_command_class
shell_class
"

fail=0
while IFS= read -r line; do
    [ -z "$line" ] && continue
    name=$(echo "$line" | sed 's/.*class_desc_t \([a-z_0-9]*\) = {.*/\1/')
    # A here-string, not `echo | grep -q`: under pipefail, grep -q exiting at
    # the first match can SIGPIPE the echo and fail the whole test.
    if ! grep -qx "$name" <<<"$ALLOWED_EXTERNAL_CLASSES"; then
        echo "NON-STATIC CLASS: $line"
        echo "    '$name' has no cross-file consumer; make it 'static const'."
        fail=1
    fi
done < <(grep -rn 'class_desc_t [a-z_0-9]* = {' "$ROOT/src" --include=*.c | grep -v 'static const' | sed "s|$ROOT/||")

# ...and the converse: an allow-listed class must actually BE external, or the
# list is stale.
for name in $ALLOWED_EXTERNAL_CLASSES; do
    if ! grep -rq "^const class_desc_t $name = {" "$ROOT/src" --include=*.c; then
        echo "STALE ALLOW-LIST ENTRY: '$name' is no longer a non-static class_desc_t"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "core-layering: FAILED — class_desc_t visibility"
    exit 1
fi
echo "core-layering: OK — every non-static class_desc_t has a cross-file consumer"

# No family knowledge in generic code.  The machine-independent machine code
# (machine.c, machine_config.c, machine_slots.c) includes no family header --
# a header under a family directory, src/machines/<family>/ -- and neither it
# nor src/core/ names a model by its id: what a machine has is its profile's
# to say.  The allow-list is the formal identification table, which maps ROM
# checksums to the models they boot (rom_table.c), and the ROM-kind token
# "lisa" (rom.c's rom_kind_name), which is a ROM family, not a model.
fail=0
GENERIC="$ROOT/src/machines/machine.c $ROOT/src/machines/machine_config.c $ROOT/src/machines/machine_slots.c"
while IFS= read -r hdr; do
    base=$(basename "$hdr")
    for f in $GENERIC; do
        if grep -q "#include \"$base\"" "$f"; then
            echo "FAMILY HEADER IN GENERIC CODE: ${f#$ROOT/} includes '$base'"
            fail=1
        fi
    done
done < <(find "$ROOT/src/machines" -mindepth 2 -name '*.h')
MODELS='plus|se30|lisa|macxl|iix|iicx|iifx|iici|iisi|q700|q900|q950|q840av|q660av|pm6100|pm7100|pm8100|pm7500|pm8500|pm9500|ans500|ans700|pmg3dt|pmg3mt'
# Every model the tree knows must be in that list, or the check goes blind.
for p in "$ROOT"/src/machines/*/*.c; do
    id=$(grep -oE '^    \.id = "[a-z0-9]+",' "$p" | head -1 | sed 's/.*"\(.*\)".*/\1/' || true)
    [ -z "$id" ] && continue
    grep -q '^const hw_profile_t' "$p" || continue
    if ! grep -qE "^($MODELS)\$" <<<"$id"; then
        echo "MODEL LIST STALE: '$id' (${p#$ROOT/}) is not in core-layering's model list"
        fail=1
    fi
done
hits=$(grep -rnE "\"($MODELS)\"" "$ROOT/src/core" $GENERIC \
    | grep -v "^$ROOT/src/core/memory/rom_table.c:" \
    | grep -vE "^$ROOT/src/core/memory/rom.c:[0-9]+:\s*return \"lisa\";" \
    | grep -vE "//.*\"($MODELS)\"" || true)
if [ -n "$hits" ]; then
    echo "MODEL ID IN GENERIC CODE:"
    echo "$hits" | sed "s|$ROOT/||;s/^/    /"
    fail=1
fi
if [ "$fail" -ne 0 ]; then
    echo "core-layering: FAILED — family knowledge in generic code"
    exit 1
fi
echo "core-layering: OK — no family header or model id in generic code"
