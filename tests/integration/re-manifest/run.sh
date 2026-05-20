#!/bin/bash
# Drive the full re.dump pipeline and validate four things:
#   1. manifest.json validates as well-formed JSON via python -m json.tool.
#   2. Required top-level keys are present.
#   3. decoded/vers/1.json round-trips through json.tool and contains the
#      expected version-shape fields.
#   4. The --no-disasm / --no-decode flags suppress disasm/ + decoded/ as
#      advertised.
set -euo pipefail

IMG="$TEST_DATA/systems/System_6_0_8.dsk"
FINDER="$IMG/partition1/System Folder/Finder"

FULL="$WORK_DIR/full"
NODISASM="$WORK_DIR/no-disasm"
NODECODE="$WORK_DIR/no-decode"
rm -rf "$FULL" "$NODISASM" "$NODECODE"

GS_STORAGE_CACHE="$STORAGE_CACHE" "$HEADLESS_BIN" \
    rom="$ROM_PATH" \
    --no-prompt --script-stdin --speed=max <<EOF
storage.probe $IMG
re.dump "$FINDER" "$FULL"
re.dump "$FINDER" "$NODISASM" --no-disasm
re.dump "$FINDER" "$NODECODE" --no-decode
storage.unmount $IMG
quit
EOF

assert_file() {
    [ -f "$1" ] || { echo "FAIL: missing $1"; exit 1; }
}
assert_no_file() {
    [ ! -e "$1" ] || { echo "FAIL: $1 should not exist"; exit 1; }
}

# --- 1. manifest.json is well-formed JSON ----------------------------------
assert_file "$FULL/manifest.json"
python3 -m json.tool "$FULL/manifest.json" > /dev/null || { echo "FAIL: manifest.json malformed"; exit 1; }

# --- 2. required top-level keys + structural sanity ------------------------
python3 - "$FULL/manifest.json" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    d = json.load(f)
for k in ("schema_version", "generator", "source", "resource_fork", "code", "resources"):
    assert k in d, f"manifest missing key: {k}"
assert d["schema_version"] == 1
assert isinstance(d["resources"], list) and len(d["resources"]) > 0
# Spot-check one resource has the expected file mapping shape.
sample = d["resources"][0]
for k in ("type", "id", "name", "attrs", "size", "files"):
    assert k in sample, f"resource entry missing key: {k}"
for fk in ("bin", "info", "disasm", "decoded"):
    assert fk in sample["files"], f"files missing key: {fk}"
PY

# --- 3. decoded vers/1.json shape ------------------------------------------
assert_file "$FULL/decoded/vers/1.json"
python3 -m json.tool "$FULL/decoded/vers/1.json" > /dev/null
python3 - "$FULL/decoded/vers/1.json" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    d = json.load(f)
for k in ("major", "minor", "bug", "stage", "region", "short", "long"):
    assert k in d, f"missing vers key: {k}"
assert isinstance(d["major"], int)
assert isinstance(d["short"], str)
PY

# --- 4. --no-disasm / --no-decode flags ------------------------------------
assert_no_file "$NODISASM/disasm"
assert_no_file "$NODISASM/symbols.txt"
assert_file "$NODISASM/decoded/vers/1.json"
assert_file "$NODISASM/manifest.json"

assert_no_file "$NODECODE/decoded"
assert_file "$NODECODE/disasm/jump-table.s"
assert_file "$NODECODE/symbols.txt"
assert_file "$NODECODE/manifest.json"

echo "re-manifest: manifest valid, decoded/vers/1.json well-formed, --no-X flags honoured"
