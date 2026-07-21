#!/usr/bin/env bash
#
# rom-manifest.sh - Generate the human-readable roms/ manifest for gs-test-data.
#
# Machine-derives every fact it can (canonical name, size, checksum/CRC, kind,
# compatible models / card id, family name) by running gs-headless
# machine.(v)rom.identify over each file, then merges a small curated notes
# table (provenance, Apple part numbers, Rev A/B, "16bpp requires this ROM")
# that identify cannot know.  This is the tool from proposal-test-rom-naming.md
# §4.4 — the manifest is regenerated, never hand-edited.
#
# Usage:
#   scripts/rom-manifest.sh [ROMS_DIR] [OUT_FILE]
#     ROMS_DIR  directory of *.rom / *.vrom files   (default: tests/data/roms)
#     OUT_FILE  markdown file to write, or - for stdout (default: -)
#
# Env:
#   HEADLESS_BIN  path to gs-headless (default: build/headless/gs-headless)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ROMS_DIR="${1:-$REPO_ROOT/tests/data/roms}"
OUT_FILE="${2:--}"
HEADLESS_BIN="${HEADLESS_BIN:-$REPO_ROOT/build/headless/gs-headless}"

[ -d "$ROMS_DIR" ]     || { echo "rom-manifest: no such dir: $ROMS_DIR" >&2; exit 1; }
[ -x "$HEADLESS_BIN" ] || { echo "rom-manifest: headless binary not built: $HEADLESS_BIN (run 'make headless')" >&2; exit 1; }

# A boot ROM is required to start headless; use any *.rom in the dir.
BOOT_ROM="$(find "$ROMS_DIR" -maxdepth 1 -name '*.rom' | sort | head -1)"
[ -n "$BOOT_ROM" ] || { echo "rom-manifest: no *.rom to boot headless with in $ROMS_DIR" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SCRIPT="$WORK/identify.script"
: > "$SCRIPT"
while IFS= read -r -d '' f; do
    base="$(basename "$f")"
    case "$base" in
        *.rom)  obj="machine.rom.identify"  ;;
        *.vrom) obj="machine.vrom.identify" ;;
        *) continue ;;
    esac
    printf 'echo GSFILE %s\n' "$base" >> "$SCRIPT"
    printf 'echo ${%s("%s")}\n' "$obj" "$f" >> "$SCRIPT"
done < <(find "$ROMS_DIR" -maxdepth 1 -type f -print0 | sort -z)
echo "quit" >> "$SCRIPT"

OUT="$WORK/identify.out"
GS_STORAGE_CACHE="$WORK/cache" "$HEADLESS_BIN" \
    rom="$BOOT_ROM" --no-prompt --speed=max "script=$SCRIPT" > "$OUT" 2>/dev/null

python3 - "$OUT" "$OUT_FILE" <<'PY'
import re, sys, os

out_path, dest = sys.argv[1], sys.argv[2]
lines = open(out_path, encoding="utf-8", errors="replace").read().splitlines()

def field(js, key):
    # Comma-free scalar fields (checksum, crc, canonical_name, card_id, size,
    # recognised). NOT for `name` / `compatible`, which can contain commas.
    m = re.search(r'(?:^|[,{])' + re.escape(key) + r':([^,}]*)', js)
    return m.group(1) if m else None

def rom_name(js):
    # rom.identify key order: ...,name:<free text>,canonical_name:...
    m = re.search(r'(?:^|,)name:(.*?),canonical_name:', js)
    return m.group(1) if m else ""

def compatible(js):
    m = re.search(r'compatible:\[([^\]]*)\]', js)
    return m.group(1) if m else ""

# Curated human notes keyed by canonical filename. Machine-derived columns
# (size/checksum/models) come from identify; this is only what identify cannot
# know: provenance, Apple part numbers, Rev A/B, the 16bpp fact.
NOTES = {
  "plus-v3-4d1f8172.rom":            "Macintosh Plus ROM Rev 3 (“Loud Harmonicas”).",
  "iix-iicx-se30-97221136.rom":      "Universal 256 KB ROM — boots IIx, IIcx and SE/30 (byte-identical across all three; formerly stored twice as SE30.rom + IIcx.rom).",
  "iifx-4147dd77.rom":               "Macintosh IIfx ROM.",
  "iici-368cadfe.rom":               "Macintosh IIci (“Aurora”) ROM.",
  "iisi-36b7fb6c.rom":               "Macintosh IIsi (“Erickson”) ROM.",
  "lisa2-revh-098917b2.rom":         "Apple Lisa 2 boot ROM rev H (interleaved 16 KB image; checksum is the Mac-style computed value, not the stored reset SSP).",
  "macxl-3a-094c82f0.rom":           "Macintosh XL boot ROM “3A” (interleaved 16 KB image).",
  "builtin-se30-video-4f71ff1a.vrom":"SE/30 onboard-video declaration ROM — a built-in video slot, not a NuBus card.",
  "mdc-8-24-revb-d1629664.vrom":     "Macintosh Display Card 8•24 (non-GC, JMFB). Apple part 341-0868, Rev B. Formerly stored twice as Apple-341-0868.vrom + 341-0868.vrom.",
  "display-card-24ac-d8daab87.vrom": "Apple Macintosh Display Card 24AC.",
  "824gc-v1.1-revb-d722b053.vrom":   "8•24 GC vROM v1.1 (16-Sep-91). Apple part 341-0266, the Rev B ROM — the ONLY 8•24 GC ROM with 16 bpp modes; catalog default.",
  "824gc-v1.0-reva-9e9857e8.vrom":   "8•24 GC vROM v1.0 (shipping). Apple part 341-0812-02, Rev A — no 16 bpp modes.",
  "824gc-v1.0a16-4740028d.vrom":     "8•24 GC vROM 1.00a16 alpha (codename “Dolphin”).",
}

# Old -> new alias table, kept permanently for grep-ability (proposal §7).
ALIASES = [
  ("Plus_v3.rom",            "plus-v3-4d1f8172.rom"),
  ("SE30.rom, IIcx.rom",     "iix-iicx-se30-97221136.rom"),
  ("4147DD77-IIfx.rom",      "iifx-4147dd77.rom"),
  ("368CADFE-IIci.rom",      "iici-368cadfe.rom"),
  ("36B7FB6C-IIsi.rom",      "iisi-36b7fb6c.rom"),
  ("Lisa/roms/098917B2-LisaH.rom", "lisa2-revh-098917b2.rom"),
  ("Lisa/roms/094C82F0-MacXL.rom", "macxl-3a-094c82f0.rom"),
  ("SE30.vrom",              "builtin-se30-video-4f71ff1a.vrom"),
  ("Apple-341-0868.vrom, 341-0868.vrom", "mdc-8-24-revb-d1629664.vrom"),
  ("display-card-24ac.vrom", "display-card-24ac-d8daab87.vrom"),
  ("Apple-341-0266.vrom",    "824gc-v1.1-revb-d722b053.vrom"),
  ("341-0812-02_1.0.vrom",   "824gc-v1.0-reva-9e9857e8.vrom"),
  ("Dolphin_1.0A16.vrom",    "824gc-v1.0a16-4740028d.vrom"),
]

rows, pending = [], None
for ln in lines:
    m = re.search(r'GSFILE (\S+)', ln)
    if m:
        pending = m.group(1); continue
    if pending is not None and '{recognised:' in ln:
        rows.append((pending, ln[ln.index('{recognised:'):])); pending = None

def human_kb(n):
    n = int(n)
    return f"{n // 1024} KB" if n % 1024 == 0 else f"{n} B"

out = []
out.append("# gs-test-data ROM manifest")
out.append("")
out.append("> **Generated by `scripts/rom-manifest.sh` — do not hand-edit.**")
out.append("> Canonical names, one flat `roms/` directory, and this table follow")
out.append("> `proposal-test-rom-naming.md`. The `.rom`/`.vrom` extension distinguishes")
out.append("> CPU ROMs from NuBus declaration ROMs (vROMs).")
out.append("")
out.append("## CPU ROMs (`*.rom`)")
out.append("")
out.append("| Canonical file | Size | Checksum | Family / hardware | Notes |")
out.append("|---|---|---|---|---|")
cpu = [(b, j) for b, j in rows if b.endswith(".rom")]
vro = [(b, j) for b, j in rows if b.endswith(".vrom")]
for base, js in sorted(cpu):
    canon = field(js, "canonical_name") or base
    size  = human_kb(field(js, "size") or "0")
    chk   = field(js, "checksum") or ""
    name  = rom_name(js)
    comp  = compatible(js)
    note  = NOTES.get(base, "")
    hw = f"{name} — models: {comp}" if comp else name
    out.append(f"| `{canon}` | {size} | `{chk}` | {hw} | {note} |")
out.append("")
out.append("## Declaration ROMs / vROMs (`*.vrom`)")
out.append("")
out.append("| Canonical file | Size | CRC | Card id | Notes |")
out.append("|---|---|---|---|---|")
for base, js in sorted(vro):
    canon = field(js, "canonical_name") or base
    size  = human_kb(field(js, "size") or "0")
    crc   = field(js, "crc") or ""
    card  = field(js, "card_id") or ""
    note  = NOTES.get(base, "")
    out.append(f"| `{canon}` | {size} | `{crc}` | `{card}` | {note} |")
out.append("")
out.append("## Legacy name aliases (for grep-ability)")
out.append("")
out.append("Old handover docs and agent notes reference the pre-rename names; this")
out.append("table maps them to the canonical files. The renames preserved git history.")
out.append("")
out.append("| Legacy name(s) | Canonical file |")
out.append("|---|---|")
for old, new in ALIASES:
    out.append(f"| `{old}` | `{new}` |")
out.append("")

text = "\n".join(out) + "\n"
if dest == "-":
    sys.stdout.write(text)
else:
    with open(dest, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"rom-manifest: wrote {len(cpu)} CPU ROM(s) + {len(vro)} vROM(s) to {dest}", file=sys.stderr)
PY
