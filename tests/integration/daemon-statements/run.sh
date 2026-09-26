#!/bin/bash
# Env supplied by the runner: HEADLESS_BIN, ROM_PATH, TEST_RESULTS_DIR, WORK_DIR.
mkdir -p "$TEST_RESULTS_DIR"
exec python3 probe.py "$HEADLESS_BIN" "$ROM_PATH" "$TEST_RESULTS_DIR"
