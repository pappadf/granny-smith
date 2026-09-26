// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// Signed-divide overflow conformance.
//
// The three most-negative-dividend divides (DIVS.W, DIVS.L 64/32, and the
// PowerPC `div`) each computed the quotient FIRST and inspected it afterwards
// to decide whether the operation overflowed.  For INT_MIN / -1 there is no
// representable quotient, so the C division itself is undefined behaviour --
// and the shipping build is emcc/WebAssembly, whose i32.div_s and i64.div_s
// TRAP on that pair by specification (WebAssembly core spec, "Numerics":
// signed division is undefined when the quotient is unrepresentable).  Two
// guest instructions could therefore kill the emulator instance in a browser.
//
// Native aarch64 and x86-64 hide the 68K cases: the host divide returns
// INT_MIN silently, the after-the-fact test then notices the overflow, and the
// architectural result comes out right anyway.  That is why the existing
// single-step vectors pass either way, and why these cases need their own
// suite: built with MODE=sanitize (-fsanitize=undefined) the OLD code reports
// "division of <min> by -1 cannot be represented in type ...", the new code is
// clean, and on wasm the difference is a trap rather than a diagnostic.
//
// The expected results are the architectural ones, not merely what we happen
// to produce.  M68000PRM DIVS: on overflow V is set and the OPERANDS ARE
// UNAFFECTED; N and Z are undefined, so they are deliberately not asserted.

#include "cpu.h"
#include "cpu_internal.h"
#include "harness.h"
#include "memory.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>

#define CODE_ADDR 0x001000u

extern void cpu_run_68000(cpu_t *cpu, uint32_t *instructions);
extern void cpu_run_68030(cpu_t *cpu, uint32_t *instructions);

// Execute exactly one instruction on the 68000 decoder.
static void run_one_68000(cpu_t *cpu) {
    uint32_t one = 1;
    cpu_run_68000(cpu, &one);
}

// Execute exactly one instruction on the 68030 decoder (DIVS.L is 68020+).
static void run_one_68030(cpu_t *cpu) {
    extern void *fpu_init(void);
    cpu->cpu_model = CPU_MODEL_68030;
    if (!cpu->fpu)
        cpu->fpu = fpu_init();
    uint32_t one = 1;
    cpu_run_68030(cpu, &one);
}

// DIVS.W #$FFFF,D0 with D0 = $80000000: -2^31 / -1 = 2^31, unrepresentable.
static void divs_w_overflow_sets_v_and_leaves_operands(void) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    memory_write_uint16(CODE_ADDR, 0x81FC); // DIVS.W #<data>,D0
    memory_write_uint16(CODE_ADDR + 2, 0xFFFF); // divisor = -1
    cpu->pc = CODE_ADDR;
    cpu->d[0] = 0x80000000u;
    cpu->overflow = 0;
    run_one_68000(cpu);
    ASSERT_EQ_INT(cpu->overflow != 0, 1); // V set on overflow
    ASSERT_EQ_INT((int)cpu->d[0], (int)0x80000000u); // operands unaffected (PRM)
}

// The non-overflowing neighbour, so the test above cannot pass vacuously.
static void divs_w_normal_divide(void) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    memory_write_uint16(CODE_ADDR, 0x81FC);
    memory_write_uint16(CODE_ADDR + 2, 0x0007); // divisor = 7
    cpu->pc = CODE_ADDR;
    cpu->d[0] = 100; // 100 / 7 = 14 rem 2
    cpu->overflow = 1;
    run_one_68000(cpu);
    ASSERT_EQ_INT(cpu->overflow != 0, 0); // V cleared
    ASSERT_EQ_INT((int)cpu->d[0], (int)((2u << 16) | 14u)); // remainder:quotient
}

// DIVS.L D2,D1:D0 with D1:D0 = $8000000000000000 and D2 = -1.
// Encoding 4C42 0C01: Dq=0, Dr=1, signed, 64-bit dividend, <ea> = D2.
static void divs_l_64bit_overflow_sets_v_and_leaves_operands(void) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    memory_write_uint16(CODE_ADDR, 0x4C42);
    memory_write_uint16(CODE_ADDR + 2, 0x0C01);
    cpu->pc = CODE_ADDR;
    cpu->d[1] = 0x80000000u; // dividend high
    cpu->d[0] = 0x00000000u; // dividend low
    cpu->d[2] = 0xFFFFFFFFu; // divisor = -1
    cpu->overflow = 0;
    run_one_68030(cpu);
    ASSERT_EQ_INT(cpu->overflow != 0, 1); // V set on overflow
    ASSERT_EQ_INT((int)cpu->d[1], (int)0x80000000u); // operands unaffected (PRM)
    ASSERT_EQ_INT((int)cpu->d[0], (int)0x00000000u);
}

// The non-overflowing neighbour for the 64-bit form.
static void divs_l_64bit_normal_divide(void) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    memory_write_uint16(CODE_ADDR, 0x4C42);
    memory_write_uint16(CODE_ADDR + 2, 0x0C01);
    cpu->pc = CODE_ADDR;
    cpu->d[1] = 0x00000000u; // dividend = 1000
    cpu->d[0] = 1000u;
    cpu->d[2] = 3u; // 1000 / 3 = 333 rem 1
    cpu->overflow = 1;
    run_one_68030(cpu);
    ASSERT_EQ_INT(cpu->overflow != 0, 0);
    ASSERT_EQ_INT((int)cpu->d[0], (int)333u); // quotient in Dq
    ASSERT_EQ_INT((int)cpu->d[1], (int)1u); // remainder in Dr
}

int main(void) {
    test_context_t *ctx = test_harness_init();
    if (!ctx) {
        fprintf(stderr, "Failed to initialize test harness\n");
        return 1;
    }

    RUN(divs_w_overflow_sets_v_and_leaves_operands);
    RUN(divs_w_normal_divide);
    RUN(divs_l_64bit_overflow_sets_v_and_leaves_operands);
    RUN(divs_l_64bit_normal_divide);

    test_harness_destroy(ctx);
    return 0;
}
