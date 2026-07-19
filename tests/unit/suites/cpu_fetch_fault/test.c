// Extension-word fetch fault: the instruction must abort before its data
// cycle.
//
// When an instruction's extension word lies in a page whose descriptor is
// invalid (A/UX demand paging), the fetch takes a deferred bus error and
// returns the all-ones sentinel. Real 68030 hardware aborts the instruction
// on the prefetch bus error and re-executes it after the Format $B retry;
// it never runs the data cycle. The emulator's write_ea_* helpers used to
// carry on with the sentinel as the displacement — CLR.L $4(A0) whose
// displacement word crossed into a non-resident page computed EA = A0 +
// 0xFFFF(-1) and wrote four zero bytes at A0-1, corrupting the byte before
// the intended target. Under A/UX 3.0.1 this destroyed an Xt heap pointer
// in xload/xlogo during every X11 session start ("/usr/lib/X11/.x11start:
// 174 Memory fault").
//
// The tests build a minimal two-level PMMU table: a code page whose last
// word is the CLR opcode, an INVALID page right after it (where the
// displacement word would be fetched from), and a mapped data page holding
// a sentinel pattern around A0. After executing the instruction, no byte
// around A0 may have changed and the bus error must have vectored.

#include "cpu.h"
#include "cpu_internal.h"
#include "harness.h"
#include "memory.h"
#include "mmu.h"
#include "test_assert.h"

#include <stdint.h>
#include <string.h>

// Logical/physical layout (identity-mapped except the hole):
//   page 0x0000: vector table (mapped)
//   page 0x3000: code page (mapped);  opcode at 0x3FFE
//   page 0x4000: INVALID (the displacement word would be at 0x4000)
//   page 0x5000: data + stack page (mapped); A0 = 0x5100, SSP = 0x5F00
#define CODE_LAST_WORD 0x3FFEu
#define DATA_A0        0x5100u
#define STACK_TOP      0x5F00u
#define HANDLER_ADDR   0x3800u

#define LEVEL_A_BASE 0x10000u
#define LEVEL_B_BASE 0x11000u

static void store_be32(uint8_t *p, uint32_t val) {
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);
    p[3] = (uint8_t)(val);
}

static void store_be16(uint8_t *p, uint16_t val) {
    p[0] = (uint8_t)(val >> 8);
    p[1] = (uint8_t)(val);
}

// Run one 68030 instruction (plus any pending exception dispatch).
static void run_one(cpu_t *cpu) {
    extern void cpu_run_68030(cpu_t * cpu, uint32_t * instructions);
    uint32_t one = 1;
    cpu_run_68030(cpu, &one);
}

// Build the two-level table and switch the MMU on. Maps pages 0x0000,
// 0x3000 and 0x5000 identity; leaves 0x4000 invalid. Returns the MMU.
static mmu_state_t *setup_mmu(cpu_t *cpu, memory_map_t *mem) {
    uint8_t *ram = ram_native_pointer(mem, 0);

    mmu_state_t *mmu = mmu_init(ram, 0x400000, 0x8000000, NULL, 0, 0x40000000, 0x50000000);
    if (!mmu)
        return NULL;

    // TC: enable, PS=4KB, IS=0, TIA=8, TIB=12 (8+12+12 = 32)
    uint32_t tc = (1u << 31) | (4u << 20) | (0u << 16) | (8u << 12) | (12u << 8);

    // Level-A entry 0 -> level-B table
    store_be32(ram + LEVEL_A_BASE, LEVEL_B_BASE | DESC_DT_TABLE4);
    // Level-B: identity page descriptors for pages 0, 3 and 5; page 4 invalid
    memset(ram + LEVEL_B_BASE, 0, 16 * 4);
    store_be32(ram + LEVEL_B_BASE + 0 * 4, 0x0000 | DESC_DT_PAGE);
    store_be32(ram + LEVEL_B_BASE + 3 * 4, 0x3000 | DESC_DT_PAGE);
    store_be32(ram + LEVEL_B_BASE + 5 * 4, 0x5000 | DESC_DT_PAGE);

    mmu->tc = tc;
    mmu->crp = ((uint64_t)DESC_DT_TABLE4 << 32) | LEVEL_A_BASE;
    mmu->enabled = true;
    g_mmu = mmu;
    cpu->mmu = mmu;
    mmu_invalidate_tlb(mmu); // drop the identity SoA entries

    // Bus error vector (vector 2, VBR=0) -> handler stub in the code page
    store_be32(ram + 8, HANDLER_ADDR);
    store_be16(ram + HANDLER_ADDR, 0x4E71); // NOP

    return mmu;
}

static void teardown_mmu(cpu_t *cpu, mmu_state_t *mmu) {
    cpu->mmu = NULL;
    g_mmu = NULL; // mmu_delete clears it too, but be explicit
    mmu_delete(mmu);
}

// Common body: place `opcode` as the last word of the code page so its
// extension word falls in the invalid page, seed a sentinel around A0,
// execute, and assert nothing around A0 changed.
static void check_no_stray_write(cpu_t *cpu, memory_map_t *mem, uint16_t opcode) {
    uint8_t *ram = ram_native_pointer(mem, 0);
    mmu_state_t *mmu = setup_mmu(cpu, mem);
    ASSERT_TRUE(mmu != NULL);

    // Sentinel pattern around A0 (A0-8 .. A0+11)
    for (int i = -8; i < 12; i++)
        ram[DATA_A0 + i] = (uint8_t)(0xC5 + i);

    store_be16(ram + CODE_LAST_WORD, opcode);

    cpu->pc = CODE_LAST_WORD;
    cpu->a[0] = DATA_A0;
    cpu->a[7] = STACK_TOP;
    cpu->supervisor = 1;

    run_one(cpu); // faulting instruction
    run_one(cpu); // give a deferred bus error a boundary to vector at

    // The data cycle must not have run: every byte around A0 unchanged.
    // (The old bug wrote 4 zero bytes at A0-1 for d16 = 0xFFFF.)
    for (int i = -8; i < 12; i++)
        ASSERT_EQ_INT(ram[DATA_A0 + i], (uint8_t)(0xC5 + i));

    // The fetch fault must have vectored to the bus-error handler
    // (Format $B frame pushed on the supervisor stack, PC at/past the stub).
    ASSERT_TRUE(cpu->pc >= HANDLER_ADDR && cpu->pc <= HANDLER_ADDR + 4);
    ASSERT_TRUE(cpu->halted == 0);

    teardown_mmu(cpu, mmu);
}

TEST(clr_l_d16_ext_word_fetch_fault_aborts_write) {
    // CLR.L $4(A0) = 0x42A8 0x0004 — displacement in the invalid page
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    cpu->cpu_model = CPU_MODEL_68030;
    check_no_stray_write(cpu, test_get_memory(test_get_active_context()), 0x42A8);
}

TEST(clr_w_d16_ext_word_fetch_fault_aborts_write) {
    // CLR.W $4(A0) = 0x4268 0x0004
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    cpu->cpu_model = CPU_MODEL_68030;
    check_no_stray_write(cpu, test_get_memory(test_get_active_context()), 0x4268);
}

TEST(clr_b_d16_ext_word_fetch_fault_aborts_write) {
    // CLR.B $4(A0) = 0x4228 0x0004
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    cpu->cpu_model = CPU_MODEL_68030;
    check_no_stray_write(cpu, test_get_memory(test_get_active_context()), 0x4228);
}

TEST(clr_l_d16_valid_ext_word_still_writes) {
    // Control: same instruction fully inside the mapped code page must
    // write its four zero bytes at A0+4 exactly.
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    memory_map_t *mem = test_get_memory(test_get_active_context());
    cpu->cpu_model = CPU_MODEL_68030;

    uint8_t *ram = ram_native_pointer(mem, 0);
    mmu_state_t *mmu = setup_mmu(cpu, mem);
    ASSERT_TRUE(mmu != NULL);

    for (int i = -8; i < 12; i++)
        ram[DATA_A0 + i] = (uint8_t)(0xC5 + i);

    store_be16(ram + 0x3F00, 0x42A8); // CLR.L $4(A0), fully in-page
    store_be16(ram + 0x3F02, 0x0004);

    cpu->pc = 0x3F00;
    cpu->a[0] = DATA_A0;
    cpu->a[7] = STACK_TOP;
    cpu->supervisor = 1;

    run_one(cpu);

    for (int i = -8; i < 12; i++) {
        uint8_t expect = (i >= 4 && i < 8) ? 0 : (uint8_t)(0xC5 + i);
        ASSERT_EQ_INT(ram[DATA_A0 + i], expect);
    }
    ASSERT_EQ_INT((int)cpu->pc, 0x3F04);

    teardown_mmu(cpu, mmu);
}

int main(void) {
    test_context_t *ctx = test_harness_init();
    if (!ctx) {
        fprintf(stderr, "Failed to initialize test harness\n");
        return 1;
    }

    RUN(clr_l_d16_ext_word_fetch_fault_aborts_write);
    RUN(clr_w_d16_ext_word_fetch_fault_aborts_write);
    RUN(clr_b_d16_ext_word_fetch_fault_aborts_write);
    RUN(clr_l_d16_valid_ext_word_still_writes);

    test_harness_destroy(ctx);
    return 0;
}
