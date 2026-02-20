#!/bin/bash
# Integration test: Machine Lifecycle (M1)
#
# Tests the system_create → run → checkpoint → system_destroy → system_create
# restore cycle introduced by Milestone 1 (Machine Abstraction Wiring).
#
# Step 1: Cold-boot, run 5M cycles, save checkpoint, quit.
# Step 2: Restore checkpoint, run 1M more cycles, quit.
# Both steps must exit cleanly (zero exit code).

set -e

# Environment variables set by the parent Makefile:
# HEADLESS_BIN, ROM_PATH, TEST_DATA, TEST_TMPDIR, STORAGE_CACHE

CHECKPOINT_FILE="$TEST_TMPDIR/lifecycle.gs"

echo "=== Machine Lifecycle Test (M1) ==="
echo "ROM: $ROM_PATH"
echo "Checkpoint: $CHECKPOINT_FILE"

# ---------------------------------------------------------------------------
# Step 1: Cold boot via system_create, run 5M cycles, save checkpoint
# ---------------------------------------------------------------------------
echo ""
echo "Step 1: Cold boot → run 5M cycles → save checkpoint"

cat > "$TEST_TMPDIR/step1.script" << 'INNER'
# Boot from ROM, run 5M cycles, save state, quit
run 5000000
save-state CHECKPOINT_PLACEHOLDER
quit
INNER
# Substitute the actual checkpoint path (avoid quoting issues in heredoc)
sed -i "s|CHECKPOINT_PLACEHOLDER|$CHECKPOINT_FILE|g" "$TEST_TMPDIR/step1.script"

GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    script="$TEST_TMPDIR/step1.script" \
    --speed=max

if [ ! -f "$CHECKPOINT_FILE" ]; then
    echo "ERROR: Checkpoint not created at $CHECKPOINT_FILE"
    exit 1
fi
echo "Checkpoint saved: $CHECKPOINT_FILE ($(wc -c < "$CHECKPOINT_FILE") bytes)"

# ---------------------------------------------------------------------------
# Step 2: Restore via system_create(checkpoint), run 1M more cycles, quit
# ---------------------------------------------------------------------------
echo ""
echo "Step 2: Restore checkpoint → run 1M cycles → quit"

cat > "$TEST_TMPDIR/step2.script" << 'INNER'
# Load saved checkpoint (exercises system_destroy + system_create restore path)
load-state CHECKPOINT_PLACEHOLDER
# Run 1M more cycles from restored state
run 1000000
quit
INNER
sed -i "s|CHECKPOINT_PLACEHOLDER|$CHECKPOINT_FILE|g" "$TEST_TMPDIR/step2.script"

GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    script="$TEST_TMPDIR/step2.script" \
    --speed=max

echo ""
echo "Machine lifecycle test PASSED"
