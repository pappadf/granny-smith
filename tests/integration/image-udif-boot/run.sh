#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# Integration test: boot from a disk stored as UDIF (see config.mk).
#
#   Step 1  convert the System 7.5 disk to .dmg, boot it to the desktop,
#           save a quick checkpoint (which references the disk) and a
#           consolidated one (which embeds it), export the modified disk as
#           .dmg and raw and compare them.
#   Step 2  a fresh process: load the quick checkpoint (the UDIF base and
#           its delta serve the drive again), delete the base, load the
#           consolidated one (it re-creates the base from its embedded
#           blocks), then boot the exported .dmg.
#
# Quick checkpoints are filed under the registered machine identity, so both
# steps share --checkpoint-dir and register the same id.
#
# Environment from the runner: HEADLESS_BIN, ROM_PATH, TEST_DATA,
# WORK_DIR (the disks and checkpoints, which are large), STORAGE_CACHE,
# TEST_VAR_ARGS.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CP_DIR="$WORK_DIR/cp"

run_step() {
    # shellcheck disable=SC2086 — TEST_VAR_ARGS is intentionally word-split
    GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
        rom="$ROM_PATH" \
        script="$SCRIPT_DIR/$1" \
        --checkpoint-dir="$CP_DIR" \
        --var TEST_DIR="$SCRIPT_DIR" \
        --var WORK_DIR="$WORK_DIR" \
        --var TEST_DATA="$TEST_DATA" \
        --var ROM="$ROM_PATH" \
        $TEST_VAR_ARGS \
        --speed=turbo
}

echo "Step 1: convert, boot from the UDIF, checkpoint, export"
run_step step1.script
echo ""
echo "Step 2: restore both checkpoints, boot the export"
run_step step2.script
echo "image-udif-boot passed"
