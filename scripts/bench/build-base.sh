#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
# build-base.sh — build gs-headless at another commit, for the CPU benchmarks'
# head-vs-base comparison (docs/guide/BENCHMARKS.md).
#
#   scripts/bench/build-base.sh <commit> <output-binary> [make args...]
#   scripts/bench/build-base.sh --wasm <commit> <output-dist-dir>
#
# Checks the commit out into a temporary worktree, builds the release headless
# binary there with the same flags as the head build (the defaults of
# Makefile.headless; extra make arguments may follow), copies it to
# <output-binary> and removes the worktree.  The build runs inside the tree
# with relative source paths, as the head build does, so both binaries name
# their checkpoint blocks alike.  With --wasm it builds the web frontend
# instead (`make ui2`) and copies app/web2/dist/ to <output-dist-dir>, for the
# browser variant (tests/e2e/web2-specs/perf-headbase.spec.ts).

set -euo pipefail

WASM=0
if [ "${1:-}" = "--wasm" ]; then
    WASM=1
    shift
fi
COMMIT="${1:?usage: build-base.sh <commit> <output-binary> [make args...]}"
OUT="${2:?usage: build-base.sh <commit> <output-binary> [make args...]}"
shift 2

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WT="$(mktemp -d "${TMPDIR:-/tmp}/gs-bench-base.XXXXXX")"
cleanup() {
    git -C "$ROOT" worktree remove --force "$WT" 2>/dev/null || rm -rf "$WT"
}
trap cleanup EXIT

git -C "$ROOT" worktree add --detach "$WT" "$COMMIT" >/dev/null
if [ "$WASM" = 1 ]; then
    make -C "$WT" ui2 "$@"
    rm -rf "$OUT"
    mkdir -p "$(dirname "$OUT")"
    cp -r "$WT/app/web2/dist" "$OUT"
else
    make -C "$WT" -f Makefile.headless -j"$(nproc)" "$@"
    mkdir -p "$(dirname "$OUT")"
    cp "$WT/build/headless/gs-headless" "$OUT"
fi
echo "build-base: $(git -C "$ROOT" rev-parse --short "$COMMIT") -> $OUT"
