// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// fpu_corpus.h — the shared byte-exactness corpus for the 601 FPU kernel.
// Included by test.c (native) and wasm_hash.c (the emcc/node build): both
// run the identical deterministic sweep over ppc_softfp and hash every
// (result, fpscr) pair.  Equal hashes across hosts IS the proposal §3.6
// byte-determinism acceptance — the kernel is pure integer code, so any
// divergence would be a toolchain bug, and this corpus is the tripwire.
//
// Dependency-free on purpose (stdint + ppc_softfp only): the WASM build
// compiles just ppc_softfp.c and this sweep, no harness.

#ifndef GS_TEST_PPC_FPU_CORPUS_H
#define GS_TEST_PPC_FPU_CORPUS_H

#include "ppc_softfp.h"

// Deterministic xorshift64* — no host randomness (determinism rule).
static inline uint64_t corpus_rng(uint64_t *s) {
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

// FNV-1a over the result stream.
static inline uint64_t corpus_mix(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xFFu;
        h *= 0x100000001B3ull;
    }
    return h;
}

// Edge operands: every IEEE class plus rounding-boundary neighbours.
static const uint64_t corpus_edges[] = {
    0x0000000000000000ull, // +0
    0x8000000000000000ull, // -0
    0x3FF0000000000000ull, // 1.0
    0xBFF0000000000000ull, // -1.0
    0x4000000000000000ull, // 2.0
    0x3FD5555555555555ull, // ~1/3
    0x0000000000000001ull, // min denormal
    0x800FFFFFFFFFFFFFull, // -max denormal
    0x0010000000000000ull, // min normal
    0x7FEFFFFFFFFFFFFFull, // max normal
    0xFFEFFFFFFFFFFFFFull, // -max normal
    0x7FF0000000000000ull, // +inf
    0xFFF0000000000000ull, // -inf
    0x7FF8000000000000ull, // default QNaN
    0x7FF0000000000001ull, // SNaN (payload 1)
    0xFFF4000012345678ull, // -SNaN with payload
    0x3CA0000000000000ull, // 2^-53
    0x4340000000000001ull, // 2^53 + ulp
    0x47E0000000000000ull, // 2^127 (single-overflow territory)
    0x3690000000000000ull, // 2^-150 (single-underflow territory)
};
#define CORPUS_N_EDGES (sizeof(corpus_edges) / sizeof(corpus_edges[0]))

// The full sweep: every op x single/double x rounding mode over the edge
// matrix, then a randomized block per op with mixed enables.  ~200k kernel
// calls; the hash covers result bits, write-suppression, and the FPSCR.
static uint64_t ppc_fpu_corpus_hash(void) {
    uint64_t h = 0xCBF29CE484222325ull;
    uint64_t frt;

    // Edge matrix: two-operand sweeps for every op (c fixed per-op inside
    // the madd family to hit product specials).
    for (int op = PPC_SF_ADD; op <= PPC_SF_NMSUB; op++) {
        for (int single = 0; single <= 1; single++) {
            for (uint32_t rn = 0; rn < 4; rn++) {
                for (unsigned i = 0; i < CORPUS_N_EDGES; i++) {
                    for (unsigned j = 0; j < CORPUS_N_EDGES; j++) {
                        uint32_t fpscr = rn;
                        uint64_t c = corpus_edges[(i + j) % CORPUS_N_EDGES];
                        int wrote =
                            ppc_sf_arith((ppc_sf_op_t)op, corpus_edges[i], corpus_edges[j], c, single, &fpscr, &frt);
                        h = corpus_mix(h, wrote ? frt : 0xDEADull);
                        h = corpus_mix(h, fpscr);
                    }
                }
            }
        }
    }

    // frsp / fctiw / the 604 estimates over the edges and each mode.
    for (uint32_t rn = 0; rn < 4; rn++) {
        for (unsigned i = 0; i < CORPUS_N_EDGES; i++) {
            uint32_t fpscr = rn;
            int wrote = ppc_sf_frsp(corpus_edges[i], &fpscr, &frt);
            h = corpus_mix(h, wrote ? frt : 0xDEADull);
            h = corpus_mix(h, fpscr);
            fpscr = rn;
            wrote = ppc_sf_fctiw(corpus_edges[i], (int)(rn & 1u), &fpscr, &frt);
            h = corpus_mix(h, wrote ? frt : 0xDEADull);
            h = corpus_mix(h, fpscr);
            fpscr = rn;
            wrote = ppc_sf_fres(corpus_edges[i], &fpscr, &frt);
            h = corpus_mix(h, wrote ? frt : 0xDEADull);
            h = corpus_mix(h, fpscr);
            fpscr = rn;
            wrote = ppc_sf_frsqrte(corpus_edges[i], &fpscr, &frt);
            h = corpus_mix(h, wrote ? frt : 0xDEADull);
            h = corpus_mix(h, fpscr);
        }
    }

    // Randomized block: biased-exponent operands, random modes and
    // exception enables (the enables change delivered results: wraps and
    // suppression must hash identically across hosts too).
    uint64_t rng = 0x601F0B12D4C0FFEEull;
    for (int n = 0; n < 20000; n++) {
        uint64_t a = corpus_rng(&rng), b = corpus_rng(&rng), c = corpus_rng(&rng);
        // Pull exponents toward the interesting ranges half the time.
        if (n & 1) {
            a = (a & ~0x7FF0000000000000ull) | ((uint64_t)(0x380 + (corpus_rng(&rng) % 0x100)) << 52);
            b = (b & ~0x7FF0000000000000ull) | ((uint64_t)(0x380 + (corpus_rng(&rng) % 0x100)) << 52);
        }
        uint32_t fpscr = (uint32_t)(corpus_rng(&rng) & (PPC_FPSCR_RN | PPC_FPSCR_VE | PPC_FPSCR_OE | PPC_FPSCR_UE |
                                                        PPC_FPSCR_ZE | PPC_FPSCR_XE));
        int op = (int)(corpus_rng(&rng) % 8u);
        int single = (int)(corpus_rng(&rng) & 1u);
        int wrote = ppc_sf_arith((ppc_sf_op_t)op, a, b, c, single, &fpscr, &frt);
        h = corpus_mix(h, wrote ? frt : 0xDEADull);
        h = corpus_mix(h, fpscr);
    }
    return h;
}

#endif // GS_TEST_PPC_FPU_CORPUS_H
