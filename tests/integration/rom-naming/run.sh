#!/bin/bash
# Canonical ROM/vROM filename conformance (proposal-test-rom-naming.md §4.3,
# re-homed onto the tooling naming grammar by
# proposal-content-addressed-rom-provisioning.md §3.6b).
#
# Enumerate every file in $TEST_DATA/roms and drive a single headless
# identify pass over all of them, then assert:
#   1. every file is RECOGNISED by machine.rom.identify / machine.vrom.identify;
#   2. each file's basename == the canonical name the tooling grammar
#      (scripts/rom_naming.py) derives from its content id — identify itself
#      reports content facts only, no filenames;
#   3. no two files share a checksum (CPU ROM) or Format-Block CRC (vROM).
#
# The identify surface is picked by extension (.rom → rom, .vrom → vrom).
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
        *.vrom) obj="machine.vrom.identify" ;;
        # The generated manifest (roms/README.md, proposal §4.4) and any other
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
echo "rom-naming: identifying $count file(s) in $ROMS_DIR"

OUT="$WORK_DIR/identify.out"
GS_STORAGE_CACHE="$STORAGE_CACHE" "$HEADLESS_BIN" \
    rom="$ROM_PATH" --no-prompt --speed=max "script=$SCRIPT" > "$OUT" 2>/dev/null

python3 - "$OUT" "$count" "$REPO_ROOT/scripts" <<'PY'
import re, sys

out_path, expected, scripts_dir = sys.argv[1], int(sys.argv[2]), sys.argv[3]
sys.path.insert(0, scripts_dir)
from rom_naming import canonical_name  # the tooling naming grammar

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
    # v2 echoes the identify JSON verbatim (quoted keys); the v1
    # tokenizer used to strip the quotes. Normalize both to unquoted.
    stripped = ln.replace('"', '')
    if pending is not None and '{recognised:' in stripped:
        js = stripped[stripped.index('{recognised:'):]
        records.append((pending, js))
        pending = None

fail = []
if len(records) != expected:
    fail.append(f"parsed {len(records)} identify results but expected {expected} "
                f"(a file may have produced an error instead of a result)")

seen_ids = {}
for base, js in records:
    if field(js, "recognised") != "true":
        fail.append(f"{base}: NOT recognised (unknown blobs may not live in roms/) -> {js}")
        continue
    # checksum for CPU ROMs, crc for vROMs — either way a content identity.
    ident = field(js, "checksum") or field(js, "crc")
    if not ident:
        fail.append(f"{base}: identify reported no content id -> {js}")
        continue
    # The canonical name is a pure function of the content id, owned by
    # tooling — the emulator only reported the content facts above.
    canon = canonical_name(ident)
    if canon is None:
        fail.append(f"{base}: content id {ident} has no canonical name in "
                    f"scripts/rom_naming.py (add the row)")
    elif canon != base:
        fail.append(f"{base}: filename != canonical name (expected {canon!r})")
    ident = ident.lower()
    if ident in seen_ids:
        fail.append(f"duplicate content id {ident}: {base} and {seen_ids[ident]}")
    else:
        seen_ids[ident] = base

if fail:
    print("=== rom-naming conformance FAILURES ===")
    for f in fail:
        print("  - " + f)
    sys.exit(1)

print(f"rom-naming: {len(records)} files OK — all recognised, canonically named, "
      f"{len(seen_ids)} unique content ids, no duplicates")
PY
