// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// wasm_hash.c — the WASM half of the FPU byte-exactness row: runs the
// shared corpus sweep (fpu_corpus.h) over the integer-only kernel and
// prints the digest.  Built with emcc and run under node by the suite's
// `wasm-check` target, which diffs this line against the native test's
// "corpus-hash:" line — the proposal §3.6 native/WASM acceptance.

#include "fpu_corpus.h"

#include <stdio.h>

int main(void) {
    printf("corpus-hash: %016llX\n", (unsigned long long)ppc_fpu_corpus_hash());
    return 0;
}
