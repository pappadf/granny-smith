#!/bin/bash
# Snapshot the System 6.0.8 Finder's CODE 1 disasm and CODE 0 jump table
# and diff against the checked-in goldens.  The goldens capture only the
# first 20 lines so future minor cpu_disasm changes that affect deeper
# bytes don't trigger a churn-y test failure — drift in the prologue is
# rare and meaningful when it happens.
set -euo pipefail

IMG="$TEST_DATA/systems/System_6_0_8.dsk"
FINDER="$IMG/partition1/System Folder/Finder"

OUT="$WORK_DIR/disasm-out"
SNAP="$WORK_DIR/snap"
GOLDEN_DIR="$(dirname "$0")/golden"
rm -rf "$OUT" "$SNAP"
mkdir -p "$OUT" "$SNAP"

GS_STORAGE_CACHE="$STORAGE_CACHE" "$HEADLESS_BIN" \
    rom="$ROM_PATH" \
    --no-prompt --script-stdin --speed=max <<EOF
storage.probe $IMG
re.disasm_code "$FINDER" 0 "$OUT/jump-table.s"
re.disasm_code "$FINDER" 1 "$OUT/CODE-0001.s"
storage.unmount $IMG
quit
EOF

# Capture exactly the first 20 lines for snapshot comparison.  cpu_disasm
# output drift further into the segment is acceptable; drift in the
# header / first few instructions is what we want to catch.
head -n 20 "$OUT/jump-table.s" > "$SNAP/jump-table.head"
head -n 20 "$OUT/CODE-0001.s"  > "$SNAP/CODE-0001.head"

for f in jump-table.head CODE-0001.head; do
    if ! diff -u "$GOLDEN_DIR/$f" "$SNAP/$f"; then
        echo "FAIL: $f drifted from golden"
        echo "  golden: $GOLDEN_DIR/$f"
        echo "  actual: $SNAP/$f"
        exit 1
    fi
done

echo "re-disasm: jump-table.head + CODE-0001.head match goldens"
