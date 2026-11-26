#!/bin/bash
# The plus-moof-track-content row (see config.mk):
#   1. the emulator writes the System 6.0.8 disk as a MOOF (make.lua);
#   2. make-track-content-moof.py puts the track content into its bits;
#   3. the Plus boots from it and test.lua runs the checks.
#
# Environment from the runner: HEADLESS_BIN, ROM_PATH, TEST_DATA, WORK_DIR,
# TEST_RESULTS_DIR, STORAGE_CACHE, TEST_VAR_ARGS.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLAIN="$WORK_DIR/plain.moof"
CONTENT="$WORK_DIR/content.moof"

cd "$WORK_DIR"
GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    script="$SCRIPT_DIR/make.lua" \
    --var SRC="$TEST_DATA/systems/System_6_0_8.dsk" \
    --var DST="$PLAIN" \
    --speed=turbo
[ -f "$PLAIN" ] || { echo "ERROR: no MOOF was written"; exit 1; }

python3 "$SCRIPT_DIR/make-track-content-moof.py" "$PLAIN" "$CONTENT"
# The same capture behind a wrapper and inside an archive: the image keeps
# the decoded inner source, and the zip member's, as its capture.
python3 -c 'import gzip, shutil, sys, zipfile
shutil.copyfileobj(open(sys.argv[1], "rb"), gzip.open(sys.argv[1] + ".gz", "wb"))
zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_DEFLATED).write(sys.argv[1], "content.moof")' \
    "$CONTENT" "$WORK_DIR/content.zip"
# The quick-checkpoint steps' capture, kept aside: test.lua moves the MOOF
# away and its consolidated restore leaves a placeholder at its path.
cp "$CONTENT" "$WORK_DIR/quick-content.moof"

cd "$SCRIPT_DIR"
# shellcheck disable=SC2086 — TEST_VAR_ARGS is intentionally word-split
GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
    rom="$ROM_PATH" \
    fd="$CONTENT" \
    --checkpoint-dir="$WORK_DIR/cp" \
    script="$SCRIPT_DIR/test.lua" \
    --var MOOF="$CONTENT" \
    --var WORK_DIR="$WORK_DIR" \
    $TEST_VAR_ARGS \
    --speed=turbo 2>&1 | tee "$TEST_RESULTS_DIR/output.log"
rc="${PIPESTATUS[0]}"
[ "$rc" -eq 0 ] || exit "$rc"

# A quick checkpoint depends on its base file, so one whose capture changed
# is refused -- even when the file's size and time stamp are the same, so
# that only the capture's fingerprint tells them apart (the two MOOFs are
# the same size: the content is edited into the bits in place).
QUICK="$WORK_DIR/quick.moof"
quick() {
    # shellcheck disable=SC2086 — TEST_VAR_ARGS is intentionally word-split
    GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN \
        rom="$ROM_PATH" \
        script="$SCRIPT_DIR/$1" \
        --checkpoint-dir="$WORK_DIR/cp-quick" \
        --var QUICK="$QUICK" \
        $TEST_VAR_ARGS \
        --speed=turbo
}
cp "$WORK_DIR/quick-content.moof" "$QUICK"
touch -d "2001-02-03 04:05:06" "$QUICK"
quick quick-save.lua
cp "$PLAIN" "$QUICK"
touch -d "2001-02-03 04:05:06" "$QUICK"
out=$(quick quick-load.lua 2>&1) || { echo "$out"; exit 1; }
echo "$out"
echo "$out" | grep -q "capture is not the one the checkpoint was saved with" || {
    echo "ERROR: the refusal did not name the changed capture"
    exit 1
}
