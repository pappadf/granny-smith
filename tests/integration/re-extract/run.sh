#!/bin/bash
# Drives the dump tool twice over the same input so we can hash both
# outputs and compare — `dump` is required to be byte-stable across runs.
# The Mac forked file is first extracted out of the HFS image via the
# emulator's storage.cp (data fork + resource fork + Finder info sidecar),
# then passed to the standalone tool.
set -euo pipefail

IMG="$TEST_DATA/aux/aux_3.0.1/hd160-with-aux-301.img"
# partition1 of this APM image is the MacOS Apple_HFS partition (the AUX
# UFS root is partition4 — distinctly NOT what we target here).  See
# storage.partmap on the same image for confirmation.
FINDER="$IMG/partition1/System Folder/Finder"

EXTRACT="$WORK_DIR/extract"
DUMP_A="$WORK_DIR/dump-a"
DUMP_B="$WORK_DIR/dump-b"
rm -rf "$EXTRACT" "$DUMP_A" "$DUMP_B"
mkdir -p "$EXTRACT"

# Pull the three artefacts out of the HFS image once.
GS_STORAGE_CACHE="$STORAGE_CACHE" "$HEADLESS_BIN" \
    rom="$ROM_PATH" \
    --no-prompt --script-stdin --speed=max <<EOF
storage.probe $IMG
storage.cp "$FINDER" "$EXTRACT/data"
storage.cp "$FINDER/rsrc/_raw" "$EXTRACT/rsrc"
storage.cp "$FINDER/finf" "$EXTRACT/finf"
storage.unmount $IMG
quit
EOF

[ -f "$EXTRACT/rsrc" ] || { echo "FAIL: storage.cp did not produce $EXTRACT/rsrc"; exit 1; }

# Identify (smoke check the parser sees the same file shape).
"$DUMP_BIN" --identify --data "$EXTRACT/data" --rsrc "$EXTRACT/rsrc"

# Run the dump twice into different output directories.
"$DUMP_BIN" --data "$EXTRACT/data" --rsrc "$EXTRACT/rsrc" --finf "$EXTRACT/finf" "$DUMP_A"
"$DUMP_BIN" --data "$EXTRACT/data" --rsrc "$EXTRACT/rsrc" --finf "$EXTRACT/finf" "$DUMP_B"

# Self-contained completeness checks — make sure the output structure is what
# downstream tooling expects before we hash everything.
for d in "$DUMP_A" "$DUMP_B"; do
    [ -f "$d/data.bin" ] || { echo "MISSING: $d/data.bin"; exit 1; }
    [ -f "$d/finder.json" ] || { echo "MISSING: $d/finder.json"; exit 1; }
    [ -d "$d/resources/CODE" ] || { echo "MISSING: $d/resources/CODE/"; exit 1; }
    [ -f "$d/resources/CODE/0.info" ] || { echo "MISSING: $d/resources/CODE/0.info"; exit 1; }
done

# Determinism: every file under dump-a must byte-match its sibling in dump-b.
DIFF_OUT=$(diff -r "$DUMP_A" "$DUMP_B" || true)
if [ -n "$DIFF_OUT" ]; then
    echo "FAIL: dump output is not deterministic across runs:"
    echo "$DIFF_OUT"
    exit 1
fi

# Cross-check that the per-resource files concatenate back to the same number
# of resource bytes as listed in the .info sidecars.  Bit-exact reconstruction
# of the raw fork would require knowing the on-disk alignment padding, which
# is intentionally not preserved by the synthetic tree.
declare -i total=0
while IFS= read -r -d '' info; do
    sz=$(grep -oE '"size":[0-9]+' "$info" | head -n1 | cut -d: -f2)
    total=$((total + sz))
done < <(find "$DUMP_A/resources" -name '*.info' -print0)

declare -i actual=0
while IFS= read -r -d '' f; do
    sz=$(stat -c %s "$f")
    actual=$((actual + sz))
done < <(find "$DUMP_A/resources" -type f ! -name '*.info' -print0)

if [ "$total" != "$actual" ]; then
    echo "FAIL: sidecar size totals ($total) != on-disk resource bytes ($actual)"
    exit 1
fi

echo "re-extract: 2 runs deterministic, $total resource bytes accounted for"
