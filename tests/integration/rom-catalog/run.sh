#!/bin/bash
# Every test-data ROM is one the core knows.
#
# Enumerate every file in $TEST_DATA/roms and drive a single headless
# identify pass over all of them, then assert:
#   1. every file is RECOGNISED by machine.rom.identify / vrom.identify /
#      prom.identify;
#   2. every CPU ROM is intact (its own stored checksum verifies) and
#      supported (its table row names an emulated model).
#
# The identify surface is picked by extension (.rom → rom, .vrom → vrom,
# .prom → prom).  Filenames are otherwise free-form: nothing here parses
# them — what a file is comes from the core alone.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
ROMS_DIR="$TEST_DATA/roms"
[ -d "$ROMS_DIR" ] || { echo "FAIL: $ROMS_DIR does not exist"; exit 1; }

# Build the headless script: for each file emit a marker line naming the file,
# then echo its identify JSON.  The shell prints values in an unquoted display
# form, but the fields we key on (recognised / checksum / crc) are comma-free,
# so they extract cleanly regardless of the free-text `name`.
SCRIPT="$WORK_DIR/identify.script"
: > "$SCRIPT"
count=0
while IFS= read -r -d '' f; do
    base="$(basename "$f")"
    case "$base" in
        *.rom)  obj="machine.rom.identify"  ;;
        *.vrom) obj="catalog.vroms.identify" ;;
        *.prom) obj="catalog.proms.identify" ;;
        # The generated manifest (roms/README.md) and any other
        # docs legitimately live here — they are not ROM blobs, so skip them.
        README.md|*.md) continue ;;
        *) echo "FAIL: unexpected non-ROM/doc file in roms/: $base"; exit 1 ;;
    esac
    printf 'echo GSFILE %s\n' "$base" >> "$SCRIPT"
    printf 'echo "${%s(\"%s\")}"\n' "$obj" "$f" >> "$SCRIPT"
    count=$((count + 1))
done < <(find "$ROMS_DIR" -maxdepth 1 -type f -print0 | sort -z)
echo "quit" >> "$SCRIPT"

[ "$count" -gt 0 ] || { echo "FAIL: no files found in $ROMS_DIR"; exit 1; }
echo "rom-catalog: identifying $count file(s) in $ROMS_DIR"

OUT="$WORK_DIR/identify.out"
GS_STORAGE_CACHE="$STORAGE_CACHE" "$HEADLESS_BIN" \
    rom="$ROM_PATH" --no-prompt --speed=turbo "script=$SCRIPT" > "$OUT" 2>/dev/null

python3 - "$OUT" "$count" <<'PY'
import re, sys

out_path, expected = sys.argv[1], int(sys.argv[2])

lines = open(out_path, encoding="utf-8", errors="replace").read().splitlines()

def field(js, key):
    m = re.search(r'(?:^|[,{])' + re.escape(key) + r':([^,}]*)', js)
    return m.group(1) if m else None

records = []          # (basename, json)
pending = None
for ln in lines:
    m = re.search(r'GSFILE (\S+)', ln)
    if m:
        pending = m.group(1)
        continue
    # The identify map echoes as JSON; normalize away the key quotes.
    stripped = ln.replace('"', '')
    if pending is not None and '{recognised:' in stripped:
        js = stripped[stripped.index('{recognised:'):]
        records.append((pending, js))
        pending = None

fail = []
if len(records) != expected:
    fail.append(f"parsed {len(records)} identify results but expected {expected} "
                f"(a file may have produced an error instead of a result)")

for base, js in records:
    if field(js, "recognised") != "true":
        fail.append(f"{base}: NOT recognised (unknown blobs may not live in roms/) -> {js}")
        continue
    if base.endswith(".rom"):
        if field(js, "intact") != "true":
            fail.append(f"{base}: its own checksum does not verify ({field(js, 'reason')}) -> {js}")
        if field(js, "supported") != "true":
            fail.append(f"{base}: recognised but not supported (no emulated model) -> {js}")

if fail:
    print("=== rom-catalog FAILURES ===")
    for f in fail:
        print("  - " + f)
    sys.exit(1)

print(f"rom-catalog: {len(records)} files OK — all recognised; every CPU ROM intact and supported")
PY
