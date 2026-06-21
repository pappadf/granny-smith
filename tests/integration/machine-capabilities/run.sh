#!/usr/bin/env bash
# machine.profile() capability-probe assertions.
#
# Runs the headless shell once, dumps machine.profile for every registered
# model, then greps each model's JSON line for the expected capability
# fields.  Each model's profile is a single JSON line containing
# "id":"<model>", so we isolate a model's line by that key.
set -euo pipefail

OUT="$WORK_DIR/profiles.txt"
SCRIPT="$WORK_DIR/profiles.script"
mkdir -p "$WORK_DIR"

MODELS="plus se30 iicx iix iifx iici iisi lisa macxl"

: > "$SCRIPT"
for m in $MODELS; do
    echo "machine.profile $m" >> "$SCRIPT"
done
echo "quit" >> "$SCRIPT"

"$HEADLESS_BIN" rom="$ROM_PATH" script="$SCRIPT" --speed=max > "$OUT" 2>&1

fail=0

# Return the single JSON line for a model id.
profile_line() {
    grep "\"id\":\"$1\"" "$OUT" | head -1
}

# assert_contains <model> <needle> <description>
assert_contains() {
    local model="$1" needle="$2" desc="$3"
    local line
    line=$(profile_line "$model")
    if [ -z "$line" ]; then
        echo "FAIL: no profile JSON for model '$model'"
        fail=1
        return
    fi
    if ! printf '%s' "$line" | grep -qF "$needle"; then
        echo "FAIL: $model: expected $desc ($needle)"
        fail=1
    fi
}

# assert_absent <model> <needle> <description>
assert_absent() {
    local model="$1" needle="$2" desc="$3"
    local line
    line=$(profile_line "$model")
    if printf '%s' "$line" | grep -qF "$needle"; then
        echo "FAIL: $model: unexpected $desc ($needle)"
        fail=1
    fi
}

# --- MMU capability kind, per model -------------------------------------
assert_contains plus  '"kind":"none"'         "mmu kind none"
assert_contains se30  '"kind":"68030_pmmu"'   "mmu kind 68030_pmmu"
assert_contains iicx  '"kind":"68030_pmmu"'   "mmu kind 68030_pmmu"
assert_contains iix   '"kind":"68030_pmmu"'   "mmu kind 68030_pmmu"
assert_contains iifx  '"kind":"68030_pmmu"'   "mmu kind 68030_pmmu"
assert_contains iici  '"kind":"68030_pmmu"'   "mmu kind 68030_pmmu"
assert_contains iisi  '"kind":"68030_pmmu"'   "mmu kind 68030_pmmu"
assert_contains lisa  '"kind":"lisa_segment"' "mmu kind lisa_segment"
assert_contains macxl '"kind":"lisa_segment"' "mmu kind lisa_segment"

# --- CPU model + FPU derivation -----------------------------------------
assert_contains plus '"model":68000'  "cpu model 68000"
assert_contains plus '"fpu":false'    "no fpu on 68000"
assert_contains se30 '"model":68030'  "cpu model 68030"
assert_contains se30 '"fpu":true'     "fpu on 68030"

# --- nubus capability ----------------------------------------------------
assert_contains plus '"nubus":false'  "plus has no nubus"
assert_contains iicx '"nubus":true'   "iicx has nubus"

# --- VROM-by-card: the SE/30-vs-IIci asymmetry --------------------------
# IIcx user video slot offers the 8·24 card, which requires a VROM file.
assert_contains iicx '"id":"mdc_8_24"'        "iicx 8·24 video card"
assert_contains iicx '"requires_vrom":true'   "iicx card needs vrom"
# IIci built-in RBV video carries its declaration in main ROM — no VROM.
assert_contains iici '"id":"builtin_rbv_video"' "iici builtin video card"
assert_absent   iici '"requires_vrom":true'     "iici card must not need vrom"

if [ "$fail" -ne 0 ]; then
    echo "--- captured profile output ---"
    cat "$OUT"
    exit 1
fi

echo "machine-capabilities: all capability assertions passed"
