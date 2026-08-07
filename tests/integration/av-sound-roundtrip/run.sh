#!/bin/bash
# Two steps (see config.mk): record through the GUI and dump the playback,
# then measure that playback against the input.
#
# Environment from the runner: HEADLESS_BIN, ROM_PATH, TEST_DATA,
# TEST_RESULTS_DIR, WORK_DIR, STORAGE_CACHE, TEST_VAR_ARGS.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UTTERANCE="$TEST_DATA/speech/sr-what-day-is-it.wav"
DECOY="$TEST_DATA/speech/sr-open-the-trash.wav"
PLAYBACK="$WORK_DIR/playback.wav"

[ -f "$UTTERANCE" ] || { echo "ERROR: test media missing: $UTTERANCE"; exit 1; }

echo "Step 1: record the utterance through the Sound control panel"
# shellcheck disable=SC2086 — TEST_VAR_ARGS is intentionally word-split
"$HEADLESS_BIN" \
    rom="$ROM_PATH" \
    model=q840av ram=16384 \
    script=record.script \
    --var WORK_DIR="$WORK_DIR" \
    --var TEST_RESULTS_DIR="$TEST_RESULTS_DIR" \
    $TEST_VAR_ARGS \
    --speed=max

[ -f "$PLAYBACK" ] || { echo "ERROR: no playback captured: $PLAYBACK"; exit 1; }

echo "Step 2: compare the playback against the input"
python3 "$SCRIPT_DIR/compare.py" "$UTTERANCE" "$DECOY" "$PLAYBACK"
