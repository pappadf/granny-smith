// 68020+ full-extension-word effective-address tests.
//
// Validates calculate_ea_full() (in src/core/cpu/cpu_internal.h) by
// executing single 68030 LEA instructions and verifying the resulting
// address loaded into An. LEA is ideal because it computes the EA
// without needing a real dereference — except for memory-indirect
// variants, which are tested against writable RAM addresses that hold
// the pointer words the kernel would chase.
//
// These cases exercise the same code path the se30 retail kernel uses
// at $10011E24-$10011E3E (BD.L + index, BS=1) and the slot dispatcher's
// pre-/post-indexed memory indirect forms.

#include "cpu.h"
#include "cpu_internal.h"
#include "harness.h"
#include "memory.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CODE_ADDR 0x001000u

// Run one 68030 instruction starting at cpu->pc.
static void run_one(cpu_t *cpu) {
    extern void cpu_run_68030(cpu_t * cpu, uint32_t * instructions);
    uint32_t one = 1;
    cpu_run_68030(cpu, &one);
}

// Upgrade the harness CPU to 68030 so the full-extension-word decoder
// path runs. cpu_init() was called with CPU_MODEL_68000; we swap models
// and allocate the FPU the 68030 path expects.
static void make_68030(cpu_t *cpu) {
    extern void *fpu_init(void);
    cpu->cpu_model = CPU_MODEL_68030;
    if (!cpu->fpu)
        cpu->fpu = fpu_init();
}

static void write_words(uint32_t addr, const uint16_t *words, int n) {
    for (int i = 0; i < n; i++)
        memory_write_uint16(addr + i * 2, words[i]);
}

// LEA <ea>,An opcode: 0100 nnn 111 mmm rrr
// - nnn: destination register (0..7) -> stored in bits [11:9]
// - 111 in [8:6]
// - mmm in [5:3], rrr in [2:0]: source EA mode/reg
static uint16_t lea_opcode(int dst_an, int src_mode, int src_reg) {
    return (uint16_t)(0x41C0 | (dst_an << 9) | (src_mode << 3) | src_reg);
}

// Helper: load instruction + ext words, set PC, set A0/D0, run once, return A<dst>.
static uint32_t exec_lea(cpu_t *cpu, int dst_an, int src_mode, int src_reg, const uint16_t *ext_and_disp,
                         int n_ext_words, uint32_t a0, uint32_t d0) {
    uint16_t buf[8];
    buf[0] = lea_opcode(dst_an, src_mode, src_reg);
    for (int i = 0; i < n_ext_words; i++)
        buf[1 + i] = ext_and_disp[i];
    write_words(CODE_ADDR, buf, 1 + n_ext_words);
    cpu->pc = CODE_ADDR;
    cpu->a[0] = a0;
    cpu->d[0] = d0;
    cpu->a[dst_an] = 0xDEADBEEF; // sentinel so we see any no-op
    run_one(cpu);
    return cpu->a[dst_an];
}

TEST(ea_full_no_indirect_BS1_BDL_index) {
    // $11004DA6(,D0.W*1) -- the exact form note 16 pointed at.
    // ext = D/A=0 Xn=0(D0) W/L=0 scale=0 full=1 BS=1 IS=0 BDsize=3 iis=0
    //     = 0x100 | 0x80 | 0x30 = 0x01B0
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    uint16_t ext[] = {0x01B0, 0x1100, 0x4DA6};
    uint32_t ea = exec_lea(cpu, 1, 6, 0, ext, 3, /*a0*/ 0, /*d0*/ 0x0040);
    // BS=1 suppresses A0; BD=$11004DA6; index D0.W sign-extends low word of D0.
    // D0.W = 0x0040 -> EA = $11004DA6 + 0x40 = $11004DE6
    ASSERT_EQ_INT((int)ea, (int)0x11004DE6u);
}

TEST(ea_full_no_indirect_BS0_BDL_index) {
    // $11000000(A0,D0.L*4)  -- full, BS=0 IS=0 BDsize=3 iis=0, D0.L*4
    // ext = D/A=0 Xn=0(D0) W/L=1 scale=2 full=1 BS=0 IS=0 BDsize=3 iis=0
    //     = 0x800 | 0x400 | 0x100 | 0x30 = 0x0D30
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    uint16_t ext[] = {0x0D30, 0x1100, 0x0000};
    uint32_t ea = exec_lea(cpu, 1, 6, 0, ext, 3, /*a0*/ 0x0000100, /*d0*/ 0x00000010);
    // EA = A0 + BD + D0*4 = 0x100 + 0x11000000 + 0x40 = 0x11000140
    ASSERT_EQ_INT((int)ea, (int)0x11000140u);
}

TEST(ea_full_no_indirect_IS1_BDL) {
    // $12345678(A0)  -- 32-bit displacement on An
    // ext = full BS=0 IS=1 BDsize=3 iis=0 = 0x0170
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    uint16_t ext[] = {0x0170, 0x0012, 0x3456};
    uint32_t ea = exec_lea(cpu, 1, 6, 0, ext, 3, /*a0*/ 0x00100000, /*d0*/ 0);
    // EA = A0 + BD = 0x100000 + 0x00123456 = 0x00223456
    ASSERT_EQ_INT((int)ea, (int)0x00223456u);
}

TEST(ea_full_no_indirect_BS1_IS1_BDL_absolute) {
    // Absolute via BS=1 IS=1 BD.L iis=0  -> treat BD as absolute address
    // ext = full BS=1 IS=1 BDsize=3 iis=0 = 0x01F0
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    uint16_t ext[] = {0x01F0, 0x00AA, 0xBBCC};
    uint32_t ea = exec_lea(cpu, 1, 6, 0, ext, 3, /*a0*/ 0xFFFFFFFF, /*d0*/ 0xFFFFFFFF);
    ASSERT_EQ_INT((int)ea, (int)0x00AABBCCu);
}

TEST(ea_full_no_indirect_BDW_negative) {
    // $FFFE(A0,D0.L*1) sign-extends BD word -> EA = A0 - 2 + D0
    // ext = full BS=0 IS=0 BDsize=2 (word) iis=0, D0.L*1
    //     = 0x800 | 0x100 | 0x20 = 0x0920
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    uint16_t ext[] = {0x0920, 0xFFFE};
    uint32_t ea = exec_lea(cpu, 1, 6, 0, ext, 2, /*a0*/ 0x2000, /*d0*/ 0x0004);
    // EA = 0x2000 + (int16)0xFFFE + 4 = 0x2000 - 2 + 4 = 0x2002
    ASSERT_EQ_INT((int)ea, (int)0x2002u);
}

TEST(ea_full_memory_indirect_preindexed) {
    // ([$400,A0,D0.W*1],$10) -- preindexed memory indirect with word OD.
    // Inner address = A0 + BD + D0  -> read longword from there.
    // Final EA      = *(inner) + OD.
    // ext = full BS=0 IS=0 BDsize=2 iis=2 (preindex + OD.W) = 0x0122
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);

    // Place the indirect pointer in RAM. A0=0x8000, D0=0x10, BD=0x400
    // -> inner = 0x8000 + 0x400 + 0x10 = 0x8410.
    // Store *0x8410 = 0x2000 (the chased pointer).
    memory_write_uint32(0x8410, 0x00002000);

    uint16_t ext[] = {0x0122, 0x0400, 0x0010};
    uint32_t ea = exec_lea(cpu, 1, 6, 0, ext, 3, /*a0*/ 0x8000, /*d0*/ 0x00000010);
    // Expected = *0x8410 + OD = 0x2000 + 0x10 = 0x2010
    ASSERT_EQ_INT((int)ea, (int)0x2010u);
}

TEST(ea_full_memory_indirect_postindexed) {
    // ([$400,A0],D0.W*1,$10) -- postindexed: index added AFTER indirect fetch.
    // Inner address = A0 + BD  ->  read longword.
    // Final EA      = *(inner) + index + OD.
    // ext = full BS=0 IS=0 BDsize=2 iis=6 (postindex + OD.W) = 0x0126
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);

    // A0=0x9000, BD=0x200  -> inner = 0x9200. Store pointer there.
    memory_write_uint32(0x9200, 0x00003000);

    uint16_t ext[] = {0x0126, 0x0200, 0x0020};
    uint32_t ea = exec_lea(cpu, 1, 6, 0, ext, 3, /*a0*/ 0x9000, /*d0*/ 0x00000040);
    // Expected = *0x9200 + D0 + OD = 0x3000 + 0x40 + 0x20 = 0x3060
    ASSERT_EQ_INT((int)ea, (int)0x3060u);
}

TEST(ea_full_memory_indirect_IS_no_index) {
    // ([$200,A0]) -- IS=1 memory indirect, no index, no outer disp.
    // Inner = A0 + BD -> read longword -> EA.
    // ext = full BS=0 IS=1 BDsize=2 iis=1 = 0x0161
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);

    memory_write_uint32(0xA200, 0xAABBCCDD);
    uint16_t ext[] = {0x0161, 0x0200};
    uint32_t ea = exec_lea(cpu, 1, 6, 0, ext, 2, /*a0*/ 0xA000, /*d0*/ 0);
    ASSERT_EQ_INT((int)ea, (int)0xAABBCCDDu);
}

TEST(ea_full_ea_words_count_advances_correctly) {
    // Regression: a full-ext source that consumes more than 1 extension
    // word must advance PC past all of {ext, BD.L} = 3 words plus the
    // opcode itself -> PC should be CODE_ADDR + 8 after the LEA.
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);

    uint16_t ext[] = {0x0D30, 0x1100, 0x0000}; // BD.L
    exec_lea(cpu, 1, 6, 0, ext, 3, /*a0*/ 0, /*d0*/ 0);
    ASSERT_EQ_INT((int)cpu->pc, (int)(CODE_ADDR + 2 + 3 * 2)); // opcode + 3 ext words
}

int main(void) {
    test_context_t *ctx = test_harness_init();
    if (!ctx) {
        fprintf(stderr, "Failed to initialize test harness\n");
        return 1;
    }

    RUN(ea_full_no_indirect_BS1_BDL_index);
    RUN(ea_full_no_indirect_BS0_BDL_index);
    RUN(ea_full_no_indirect_IS1_BDL);
    RUN(ea_full_no_indirect_BS1_IS1_BDL_absolute);
    RUN(ea_full_no_indirect_BDW_negative);
    RUN(ea_full_memory_indirect_preindexed);
    RUN(ea_full_memory_indirect_postindexed);
    RUN(ea_full_memory_indirect_IS_no_index);
    RUN(ea_full_ea_words_count_advances_correctly);

    test_harness_destroy(ctx);
    return 0;
}
