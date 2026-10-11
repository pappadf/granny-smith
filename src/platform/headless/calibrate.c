// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// calibrate.c
// gs-headless --calibrate: a yardstick for the host, for the CPU benchmarks'
// metric C (normalized MIPS, docs/guide/BENCHMARKS.md).  Three fixed
// kernels, each timed for about half a second of process CPU time, report
// nanoseconds per iteration:
//
//   alu     issue width and frequency: 8 independent integer chains of add,
//           xor and shift, unrolled
//   chase   cache and memory latency: a dependent pointer chase through a
//           1 MB random cycle (the emulator's page tables and RAM)
//   interp  indirect-branch prediction and dispatch: a 64-opcode bytecode
//           interpreter, a switch over a fixed pseudo-random 64 KB program
//           with its register file in memory -- the shape of the CPU cores'
//           dispatch loops
//
// score = geomean(ref/alu, ref/chase, ref/interp), 1.0 on the reference host.
//
// THE KERNELS ARE FIXED.  A benchmark trend divides by this score, so any
// change to a kernel, its data or the reference values breaks every trend
// line drawn so far: such a change bumps CALIBRATE_VERSION (the trend keys
// points by it), and nothing else in this file changes without one.

#include "headless.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// The kernels' version: part of every trend key.
#define CALIBRATE_VERSION 1

// The reference host's ns/iteration (score 1.0): the machine version 1 was
// written on, an "Intel Xeon Processor @ 2.10GHz" cloud container, medians of
// five runs of the release build.  Only a scale: trends compare a model with
// itself.
#define REF_ALU_NS    1.89
#define REF_CHASE_NS  6.77
#define REF_INTERP_NS 2.27

// Kernel sizes (fixed with the version).
#define CHASE_ENTRIES (1u << 18) // 1 MB of uint32_t
#define INTERP_PROG   (1u << 16) // 64 KB of opcodes
#define INTERP_MEM    1024u // the interpreter's data memory, in words

// Per kernel: best of this many timed runs, each about TARGET_NS long.
#define RUNS      3
#define TARGET_NS 170000000.0

// A fixed pseudo-random sequence (64-bit LCG, Knuth's MMIX constants).
static uint64_t lcg_state;

// Next 32 pseudo-random bits.
static uint32_t lcg_next(void) {
    lcg_state = lcg_state * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(lcg_state >> 32);
}

// Process CPU time in nanoseconds.
static double cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

// Keeps a result alive past the optimizer.
static volatile uint64_t sink;

// --- alu ---------------------------------------------------------------------

// `iters` rounds of 8 independent add/xor/shift chains.
static void kernel_alu(uint64_t iters) {
    uint64_t a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
    for (uint64_t i = 0; i < iters; i++) {
        a = (a + i) ^ (a << 3);
        b = (b + i) ^ (b >> 5);
        c = (c + i) ^ (c << 7);
        d = (d + i) ^ (d >> 11);
        e = (e + i) ^ (e << 13);
        f = (f + i) ^ (f >> 17);
        g = (g + i) ^ (g << 19);
        h = (h + i) ^ (h >> 23);
    }
    sink = a + b + c + d + e + f + g + h;
}

// --- chase -------------------------------------------------------------------

static uint32_t *chase_next;

// One random cycle through every entry (Sattolo's shuffle), fixed seed.
static void chase_init(void) {
    chase_next = malloc(CHASE_ENTRIES * sizeof *chase_next);
    if (!chase_next) {
        fprintf(stderr, "calibrate: out of memory\n");
        exit(1);
    }
    for (uint32_t i = 0; i < CHASE_ENTRIES; i++)
        chase_next[i] = i;
    lcg_state = 0x6368617365ull; // "chase"
    for (uint32_t i = CHASE_ENTRIES - 1; i > 0; i--) {
        uint32_t j = lcg_next() % i; // j < i: one cycle
        uint32_t t = chase_next[i];
        chase_next[i] = chase_next[j];
        chase_next[j] = t;
    }
}

// `iters` dependent loads.
static void kernel_chase(uint64_t iters) {
    uint32_t p = 0;
    for (uint64_t i = 0; i < iters; i++)
        p = chase_next[p];
    sink = p;
}

// --- interp ------------------------------------------------------------------

static uint8_t *interp_prog;

// The program: 64 opcodes, uniformly pseudo-random, fixed seed.
static void interp_init(void) {
    interp_prog = malloc(INTERP_PROG);
    if (!interp_prog) {
        fprintf(stderr, "calibrate: out of memory\n");
        exit(1);
    }
    lcg_state = 0x696e74657270ull; // "interp"
    for (uint32_t i = 0; i < INTERP_PROG; i++)
        interp_prog[i] = (uint8_t)(lcg_next() >> 26);
}

// The interpreter's state, in memory like a CPU core's register file.
typedef struct {
    uint32_t r[16];
    uint32_t mem[INTERP_MEM];
    uint32_t pc;
    uint32_t flag;
} interp_t;

// Operand registers from the opcode byte and the PC: a and b in 0..15.
#define RA(op)  st->r[((op) + st->pc) & 15]
#define RB(op)  st->r[((op) * 7 + st->pc) & 15]
#define MEMW(x) st->mem[(x) & (INTERP_MEM - 1)]

// `iters` opcodes through a switch dispatch.
static void kernel_interp(interp_t *st, uint64_t iters) {
    const uint8_t *prog = interp_prog;
    for (uint64_t i = 0; i < iters; i++) {
        uint32_t op = prog[st->pc & (INTERP_PROG - 1)];
        st->pc++;
        // clang-format off
        switch (op) {
        case 0:  RA(op) += RB(op); break;
        case 1:  RA(op) -= RB(op); break;
        case 2:  RA(op) ^= RB(op); break;
        case 3:  RA(op) &= RB(op) | 1; break;
        case 4:  RA(op) |= RB(op); break;
        case 5:  RA(op) <<= RB(op) & 7; break;
        case 6:  RA(op) >>= RB(op) & 7; break;
        case 7:  RA(op) = RB(op) * 2654435761u; break;
        case 8:  RA(op) = MEMW(RB(op)); break;
        case 9:  MEMW(RB(op)) = RA(op); break;
        case 10: RA(op) += MEMW(st->pc); break;
        case 11: MEMW(st->pc) ^= RA(op); break;
        case 12: st->flag = RA(op) < RB(op); break;
        case 13: st->flag = RA(op) == RB(op); break;
        case 14: if (st->flag) st->pc += 3; break;
        case 15: if (!st->flag) st->pc += 1; break;
        case 16: RA(op) = ~RA(op); break;
        case 17: RA(op) = -RB(op); break;
        case 18: RA(op) += 1; break;
        case 19: RA(op) -= 1; break;
        case 20: RA(op) = (RA(op) << 8) | (RA(op) >> 24); break;
        case 21: RA(op) = (RA(op) >> 8) | (RA(op) << 24); break;
        case 22: RA(op) = (uint16_t)RB(op); break;
        case 23: RA(op) = (int32_t)(int16_t)RB(op); break;
        case 24: RA(op) = (int32_t)(int8_t)RB(op); break;
        case 25: RA(op) = RB(op) >> 16; break;
        case 26: st->flag = (RA(op) & 0x80000000u) != 0; break;
        case 27: st->flag = (RA(op) & 1) != 0; break;
        case 28: RA(op) = st->flag ? RB(op) : RA(op); break;
        case 29: RA(op) += st->flag; break;
        case 30: MEMW(RA(op) + 1) = RB(op); break;
        case 31: RA(op) = MEMW(RA(op) + 1); break;
        case 32: RA(op) = RA(op) + RB(op) + st->flag; break;
        case 33: RA(op) = RA(op) - RB(op) - st->flag; break;
        case 34: { uint32_t t = RA(op); RA(op) = RB(op); RB(op) = t; } break;
        case 35: RA(op) = RB(op) & 0xFF; break;
        case 36: RA(op) = RB(op) & 0xFFFF; break;
        case 37: RA(op) ^= 0x5A5A5A5Au; break;
        case 38: RA(op) += 0x01010101u; break;
        case 39: RA(op) = (RA(op) & 0xFFFF0000u) | (RB(op) & 0xFFFF); break;
        case 40: st->flag = RA(op) > RB(op); break;
        case 41: st->flag = RA(op) != 0; break;
        case 42: if (st->flag) st->pc -= 2; break;
        case 43: RA(op) = (RA(op) * 5) >> 1; break;
        case 44: RA(op) = RB(op) / ((RA(op) & 7) + 1); break;
        case 45: RA(op) = RB(op) % ((RA(op) & 15) + 1); break;
        case 46: MEMW(st->pc + RA(op)) += 1; break;
        case 47: RA(op) = MEMW(st->pc + RB(op)) ^ RA(op); break;
        case 48: RA(op) = (RA(op) >> 1) | (st->flag << 31); break;
        case 49: RA(op) = (RA(op) << 1) | st->flag; break;
        case 50: st->flag = __builtin_popcount(RA(op)) & 1; break;
        case 51: RA(op) = RA(op) ? (uint32_t)__builtin_clz(RA(op)) : 32; break;
        case 52: RA(op) = RB(op) + 4; break;
        case 53: RA(op) = RB(op) - 4; break;
        case 54: RA(op) = RA(op) & ~RB(op); break;
        case 55: RA(op) = RA(op) | 0x80000000u; break;
        case 56: RA(op) = RA(op) & 0x7FFFFFFFu; break;
        case 57: RA(op) = (RA(op) ^ RB(op)) >> 3; break;
        case 58: RA(op) = RA(op) + (RB(op) << 2); break;
        case 59: RA(op) = RA(op) + (RB(op) >> 2); break;
        case 60: st->flag = (RA(op) & RB(op)) == 0; break;
        case 61: st->pc += RA(op) & 3; break;
        case 62: RA(op) = st->pc; break;
        default: RA(op) = RA(op) * 3 + RB(op); break;
        }
        // clang-format on
    }
}

// --- timing ------------------------------------------------------------------

// Which kernel time_kernel() runs.
typedef enum { K_ALU, K_CHASE, K_INTERP } kernel_t;

static interp_t interp_state;

// Run `k` for `iters` iterations and return the CPU nanoseconds it took.
static double run_kernel(kernel_t k, uint64_t iters) {
    double t0 = cpu_ns();
    switch (k) {
    case K_ALU:
        kernel_alu(iters);
        break;
    case K_CHASE:
        kernel_chase(iters);
        break;
    case K_INTERP:
        kernel_interp(&interp_state, iters);
        break;
    }
    return cpu_ns() - t0;
}

// Nanoseconds per iteration of `k`: size a run to TARGET_NS from a short
// probe, then the best of RUNS such runs.
static double time_kernel(kernel_t k) {
    uint64_t iters = 1u << 16;
    double t = 0;
    // Grow the probe until it is long enough to scale from (>= 10 ms).
    while ((t = run_kernel(k, iters)) < 1e7)
        iters *= 4;
    iters = (uint64_t)((double)iters * TARGET_NS / t) + 1;
    double best = 0;
    for (int i = 0; i < RUNS; i++) {
        double ns = run_kernel(k, iters) / (double)iters;
        if (i == 0 || ns < best)
            best = ns;
    }
    return best;
}

// --calibrate: run the kernels and print one @@CALIB JSON line.
int hl_calibrate(void) {
    chase_init();
    interp_init();
    double alu = time_kernel(K_ALU);
    double chase = time_kernel(K_CHASE);
    double interp = time_kernel(K_INTERP);
    // Geometric mean of the three speeds relative to the reference host.
    double score = cbrt((REF_ALU_NS / alu) * (REF_CHASE_NS / chase) * (REF_INTERP_NS / interp));
    printf("@@CALIB {\"calib_v\":%d,\"alu_ns\":%.4f,\"chase_ns\":%.4f,\"interp_ns\":%.4f,\"score\":%.4f}\n",
           CALIBRATE_VERSION, alu, chase, interp, score);
    free(chase_next);
    free(interp_prog);
    return 0;
}
