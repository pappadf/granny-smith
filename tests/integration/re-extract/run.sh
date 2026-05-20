#!/bin/bash
# Drives the headless emulator twice over the same path so we can hash both
# outputs and compare — re.dump is required to be byte-stable across runs.
set -euo pipefail

IMG="$TEST_DATA/aux/aux_3.0.1/hd160-with-aux-301.img"
# partition1 of this APM image is the MacOS Apple_HFS partition (the AUX
# UFS root is partition4 — distinctly NOT what we target here).  See
# storage.partmap on the same image for confirmation.
FINDER="$IMG/partition1/System Folder/Finder"

DUMP_A="$WORK_DIR/dump-a"
DUMP_B="$WORK_DIR/dump-b"
rm -rf "$DUMP_A" "$DUMP_B"

run_dump() {
    local dst="$1"
    GS_STORAGE_CACHE="$STORAGE_CACHE" "$HEADLESS_BIN" \
        rom="$ROM_PATH" \
        --no-prompt --script-stdin --speed=max <<EOF
storage.probe $IMG
re.identify "$FINDER"
re.dump "$FINDER" "$dst"
storage.unmount $IMG
quit
EOF
}

run_dump "$DUMP_A"
run_dump "$DUMP_B"

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
    echo "FAIL: re.dump output is not deterministic across runs:"
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
