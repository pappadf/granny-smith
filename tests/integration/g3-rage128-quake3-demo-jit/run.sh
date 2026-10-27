#!/bin/bash
# g3-rage128-quake3-demo with the translation tier (see config.mk): its
# test.script under the wasm build of the headless program, run by Node,
# with CPU_ENGINE=jit.
#
# Environment from the runner: ROM_PATH, TEST_DATA, TEST_RESULTS_DIR,
# WORK_DIR, EXTRA_MEDIA, STORAGE_CACHE, TEST_VAR_ARGS.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NODE_BIN="$ROOT/build/headless-node/gs-headless"
# Node has no job thread or I/O worker for it: run both inline, by flag
# (left to its own fallback, scheduler.run in an inline script retires
# nothing)
NODE_FLAGS="--jobs=inline --io=sync"

if command -v emcc >/dev/null 2>&1 && command -v node >/dev/null 2>&1; then
    echo "building the wasm headless program (make -f Makefile.headless TARGET=node)"
    make -C "$ROOT" -f Makefile.headless TARGET=node -j"$(nproc)" > "$TEST_RESULTS_DIR/build.log" 2>&1 ||
        { echo "the wasm build failed:"; tail -20 "$TEST_RESULTS_DIR/build.log"; exit 1; }
elif [ ! -x "$NODE_BIN" ] || ! command -v node >/dev/null 2>&1; then
    echo "skip: no wasm headless program and no emcc/node to build one"
    exit 0
fi

cd "$SCRIPT_DIR/../g3-rage128-quake3-demo" || exit 1
# shellcheck disable=SC2086 — NODE_FLAGS, TEST_VAR_ARGS are intentionally word-split
GS_STORAGE_CACHE="$STORAGE_CACHE" "$NODE_BIN" $NODE_FLAGS \
    rom="$ROM_PATH" model=pmg3dt ram=262144 \
    script=test.script \
    --var WORK_DIR="$WORK_DIR" \
    --var TEST_RESULTS_DIR="$TEST_RESULTS_DIR" \
    --var ROM="$ROM_PATH" \
    --var TEST_DATA="$TEST_DATA" \
    --var EXTRA_MEDIA="$EXTRA_MEDIA" \
    --var ROW_NAME=g3-rage128-quake3-demo-jit \
    --var CPU_ENGINE=jit \
    $TEST_VAR_ARGS \
    --speed=turbo 2>&1 | tee "$TEST_RESULTS_DIR/output.log"
exit "${PIPESTATUS[0]}"
