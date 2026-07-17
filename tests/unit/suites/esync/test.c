// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// VIA E-clock synchronization penalty unit test (proposal-via-eclock-sync).
//
// Pins the boundary math and the two properties that make E-sync faithful:
//
//  1. A single access pays the time to the next E boundary — penalty in
//     (0, E], phase-accurate on the x256 fixed-point grid, exact for both
//     integer (15.6672 MHz → 20.00 cycles) and non-integer (25 MHz →
//     31.91 cycles) cycles-per-period machines.
//  2. Back-to-back polling loops (instruction time < one E period) lock to
//     exactly one access per E period, independent of the loop's own cycle
//     cost — the CPU-independence the ROMs' delay loops rely on. Long-run
//     rate stays exact (no cumulative drift from per-access rounding).
//
// Header-only: the globals the charging wrapper uses are defined here.

#include "memory.h"
#include "test_assert.h"

#include <stdint.h>

// --- globals referenced by the memory.h inline helpers ----------------------
uint32_t g_io_penalty_remainder = 0;
uint32_t g_io_phantom_instructions = 0;
uint32_t g_io_cpi_x256 = 0;
uint32_t *g_sprint_burndown_ptr = NULL;
uint64_t g_sprint_base_cycles = 0;
uint32_t g_sprint_frac_x256 = 0;
uint32_t g_sprint_total_slots = 0;
uint32_t g_esync_period_x256 = 0;

// E period x256 for a CPU frequency (the scheduler_set_frequency formula)
static uint32_t period_x256(uint32_t freq_hz) {
    return (uint32_t)(((uint64_t)freq_hz * 256 + 783360 / 2) / 783360);
}

// ============================================================================
// 1. Pure boundary math
// ============================================================================

TEST(test_penalty_bounds_and_phase) {
    // GLUE machines: 15.6672 MHz = exactly 20 cycles per E period
    uint32_t p = period_x256(15667200);
    ASSERT_EQ_INT((int)p, 20 * 256);

    // On a boundary: pay a full period (an access never completes in the
    // instant it starts)
    ASSERT_EQ_INT((int)memory_esync_penalty_cycles(0, p), 20);
    ASSERT_EQ_INT((int)memory_esync_penalty_cycles(20, p), 20);
    // Mid-period: pay the remainder
    ASSERT_EQ_INT((int)memory_esync_penalty_cycles(1, p), 19);
    ASSERT_EQ_INT((int)memory_esync_penalty_cycles(19, p), 1);
    ASSERT_EQ_INT((int)memory_esync_penalty_cycles(41, p), 19);

    // Penalty is always in (0, period+rounding] for a spread of phases
    for (uint64_t now = 0; now < 400; now++) {
        uint32_t pen = memory_esync_penalty_cycles(now, p);
        ASSERT_TRUE(pen >= 1 && pen <= 20);
    }
}

TEST(test_nonint_period_no_drift) {
    // IIci: 25 MHz = 31.914... cycles per E period; the x256 grid must not
    // accumulate drift — N periods span N*period within rounding.
    uint32_t p = period_x256(25000000);
    ASSERT_EQ_INT((int)p, 8170); // 25e6*256/783360 rounded

    // Walk boundary-to-boundary 10,000 times: each step's penalty lands the
    // clock on the next grid point; the total must match the ideal length.
    uint64_t now = 0;
    for (int i = 0; i < 10000; i++)
        now += memory_esync_penalty_cycles(now, p);
    double ideal = 10000.0 * 8170.0 / 256.0;
    double err = (double)now - ideal;
    ASSERT_TRUE(err > -128 && err < 128); // sub-period cumulative error only
}

TEST(test_tight_loop_locks_to_one_access_per_period) {
    // The ROM delay idiom: access, then a few cycles of instructions, then
    // the next access. As long as the instruction time is under one E
    // period, iterations must lock to exactly one E period each — for ANY
    // instruction cost — on every machine frequency.
    static const uint32_t freqs[] = {15667200, 20000000, 25000000, 40000000};
    for (unsigned f = 0; f < sizeof(freqs) / sizeof(freqs[0]); f++) {
        uint32_t p = period_x256(freqs[f]);
        for (uint32_t instr_cycles = 2; instr_cycles + 2 < p / 256; instr_cycles += 5) {
            uint64_t now = 12345; // arbitrary phase
            int iters = 2000;
            uint64_t start = now;
            for (int i = 0; i < iters; i++) {
                now += memory_esync_penalty_cycles(now, p); // access completes
                now += instr_cycles; // DBF etc.
            }
            // Total spspan = iters * one E period + the loop's own tail,
            // within rounding — CPU-speed-independent pacing.
            double ideal = (double)iters * p / 256.0;
            double got = (double)(now - start) - instr_cycles; // last tail
            double err = got - ideal;
            ASSERT_TRUE(err > -(double)p / 256.0 && err < (double)p / 256.0);
        }
    }
}

// ============================================================================
// 2. The charging wrapper (sprint-slot integration)
// ============================================================================

TEST(test_wrapper_charges_via_penalty_slots) {
    // Simulate a sprint: CPI 4 (x256 = 1024), base cycles 0, 1000-slot budget.
    uint32_t burndown = 1000;
    g_io_cpi_x256 = 4 << 8;
    g_io_penalty_remainder = 0;
    g_io_phantom_instructions = 0;
    g_sprint_base_cycles = 0;
    g_sprint_frac_x256 = 0;
    g_sprint_total_slots = 1000;
    g_sprint_burndown_ptr = &burndown;
    g_esync_period_x256 = period_x256(15667200); // 20-cycle grid

    // First access at slot 0 (now = 0): pays a full 20-cycle period = 5
    // slots of burndown at CPI 4.
    memory_io_esync_penalty();
    ASSERT_EQ_INT((int)burndown, 995);

    // Two instructions execute (the sprint consumes 2 more slots)...
    burndown -= 2;
    // ...next access: now = (1000-993)*4 = 28 cycles → 12 to the next
    // boundary (40) → 3 slots.
    memory_io_esync_penalty();
    ASSERT_EQ_INT((int)burndown, 990);

    // Disabled paths: no CPI (no sprint armed) or no grid → no charge
    g_io_cpi_x256 = 0;
    memory_io_esync_penalty();
    ASSERT_EQ_INT((int)burndown, 990);
    g_io_cpi_x256 = 4 << 8;
    g_esync_period_x256 = 0;
    memory_io_esync_penalty();
    ASSERT_EQ_INT((int)burndown, 990);

    g_sprint_burndown_ptr = NULL;
}

TEST(test_wrapper_fractional_cpi) {
    // Accelerated-mode effective CPI 1.5 (x256 = 384): a 20-cycle penalty
    // burns floor(20 / 1.5) = 13 slots and carries the remaining half slot
    // (x256 cycles: 5120 - 13*384 = 128) into the next charge.
    uint32_t burndown = 1000;
    g_io_cpi_x256 = 384;
    g_io_penalty_remainder = 0;
    g_io_phantom_instructions = 0;
    g_sprint_base_cycles = 0;
    g_sprint_frac_x256 = 0;
    g_sprint_total_slots = 1000;
    g_sprint_burndown_ptr = &burndown;
    g_esync_period_x256 = 0; // charge raw penalties, not the E grid

    memory_io_penalty(20);
    ASSERT_EQ_INT((int)burndown, 1000 - 13);
    ASSERT_EQ_INT((int)g_io_penalty_remainder, 20 * 256 - 13 * 384); // 128

    // Second 20-cycle penalty: (5120 + 128) / 384 = 13 slots, remainder 256
    // x256 cycles (one whole cycle banked toward the next burn) — no penalty
    // time is ever dropped at fractional CPIs.
    memory_io_penalty(20);
    ASSERT_EQ_INT((int)burndown, 1000 - 26);
    ASSERT_EQ_INT((int)g_io_penalty_remainder, 256);

    g_sprint_burndown_ptr = NULL;
}

// ============================================================================

int main(void) {
    RUN(test_penalty_bounds_and_phase);
    RUN(test_nonint_period_no_drift);
    RUN(test_tight_loop_locks_to_one_access_per_period);
    RUN(test_wrapper_charges_via_penalty_slots);
    RUN(test_wrapper_fractional_cpi);
    fprintf(stderr, "esync: all tests passed\n");
    return 0;
}
