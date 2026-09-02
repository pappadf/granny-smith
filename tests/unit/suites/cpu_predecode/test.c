// 68K predecoded-executor unit tests (proposal-predecoded-interpreter-cores.md
// §9.2, the cpu_predecode suite).  Hand-assembled programs in the harness's
// RAM, run through cpu_run_sprint with the predecoded executor enabled, and
// checked against the switch core where the answer is "the same timeline".

#include "cpu.h"
#include "cpu_internal.h"
#include "cpu_pd_ids.h"
#include "harness.h"
#include "memory.h"
#include "predecode.h"
#include "test_assert.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static test_context_t *CTX;
static cpu_t *CPU;

// === Helpers ================================================================

// Write big-endian words at addr (the guest path: lands through the code-
// page marks like a store from the program would).
static void emit(uint32_t addr, int n, ...) {
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        uint16_t w = (uint16_t)va_arg(ap, int);
        memory_write_uint16(addr + 2u * (uint32_t)i, w);
    }
    va_end(ap);
}

// Reset the register file to a known state at pc, supervisor mode, SSP.
static void reset_cpu(uint32_t pc) {
    for (int i = 0; i < 8; i++) {
        cpu_set_dn(CPU, i, 0);
        cpu_set_an(CPU, i, 0);
    }
    cpu_set_an(CPU, 7, 0x3000);
    cpu_set_ssp(CPU, 0x3000);
    cpu_set_usp(CPU, 0x2F00);
    cpu_set_sr(CPU, 0x2700);
    cpu_set_pc(CPU, pc);
    CPU->instruction_pc = pc;
    CPU->ir = 0;
    CPU->ir_pc = 0;
    CPU->last_bus_error_pc = 0;
}

// Run n instructions in sprints of `budget`.
static void run(uint32_t n, uint32_t budget) {
    while (n > 0) {
        uint32_t b = n < budget ? n : budget;
        uint32_t left = b;
        cpu_run_sprint(CPU, &left);
        n -= b - left;
        if (left == b)
            break; // nothing retired (stopped)
    }
}

// The block for a RAM page, if the pool holds one (NULL otherwise).
static pd_block_t *block_of(uint32_t page) {
    uintptr_t base = g_supervisor_read[page];
    if (!base)
        return NULL;
    return predecode_lookup((uint8_t *)(base + (page << PAGE_SHIFT)), PD_ARCH_68K);
}

// The POD part of cpu_t (everything before the pointers).
#define CPU_POD_SIZE offsetof(cpu_t, mmu)

// === Programs ===============================================================

// A mixed loop: ALU, memory stores, immediates, compare/branch, DBRA, LEA,
// JSR/LINK/UNLK/RTS, a stack push/pop, then a self-branch to park.
#define PROG_BASE 0x1000u
static void emit_program(void) {
    emit(0x1000, 5, 0x7000, 0x7205, 0x207C, 0x0000, 0x2000); // MOVEQ #0,D0; MOVEQ #5,D1; MOVEA.L #$2000,A0
    emit(0x100A, 1, 0x2248); // MOVEA.L A0,A1
    emit(0x100C, 2, 0xD081, 0x30C1); // loop: ADD.L D1,D0; MOVE.W D1,(A0)+
    emit(0x1010, 3, 0x0680, 0x0000, 0x0003); // ADDI.L #3,D0
    emit(0x1016, 3, 0xB081, 0x6602, 0x4E71); // CMP.L D1,D0; BNE.S skip; NOP
    emit(0x101C, 3, 0x4A80, 0x51C9, 0xFFEC); // skip: TST.L D0; DBRA D1,loop
    emit(0x1022, 3, 0x43E8, 0x0010, 0x2611); // LEA 16(A0),A1; MOVE.L (A1),D3
    emit(0x1028, 3, 0x4EB9, 0x0000, 0x1040); // JSR $1040
    emit(0x102E, 4, 0x2F00, 0x4EB9, 0x0000, 0x1040); // MOVE.L D0,-(SP); JSR $1040
    emit(0x1036, 2, 0x201F, 0x60FE); // MOVE.L (SP)+,D0; BRA.S self
    emit(0x1040, 4, 0x4E56, 0xFFF8, 0x282E, 0x0008); // sub: LINK A6,#-8; MOVE.L 8(A6),D4
    emit(0x1048, 3, 0xE288, 0x4E5E, 0x4E75); // LSR.L #1,D0; UNLK A6; RTS
}
#define PROG_INSTRUCTIONS 80

// === Tests ==================================================================

// The program computes the same registers on both executors, and the
// predecoded run actually used the cache.
TEST(test_basic_loop) {
    emit_program();
    predecode_set_enabled(false);
    reset_cpu(PROG_BASE);
    run(PROG_INSTRUCTIONS, 1000);
    uint32_t ref_d0 = cpu_get_dn(CPU, 0), ref_d3 = cpu_get_dn(CPU, 3), ref_d4 = cpu_get_dn(CPU, 4);
    uint32_t ref_pc = cpu_get_pc(CPU);
    ASSERT_EQ_INT(0x1038, (int)ref_pc); // parked

    predecode_set_enabled(true);
    predecode_reset();
    reset_cpu(PROG_BASE);
    run(PROG_INSTRUCTIONS, 1000);
    ASSERT_EQ_INT((int)ref_d0, (int)cpu_get_dn(CPU, 0));
    ASSERT_EQ_INT((int)ref_d3, (int)cpu_get_dn(CPU, 3));
    ASSERT_EQ_INT((int)ref_d4, (int)cpu_get_dn(CPU, 4));
    ASSERT_EQ_INT((int)ref_pc, (int)cpu_get_pc(CPU));
    ASSERT_TRUE(g_pd_blocks_live >= 1);
    ASSERT_TRUE(g_pd_stats.decodes > 10);
    predecode_set_enabled(false);
}

// Differential run: the whole POD cpu_t and the touched RAM are identical
// between the switch core (one sprint) and the predecoded core at every
// sprint length and elision level (§5.4 in miniature).
TEST(test_differential) {
    emit_program();
    predecode_set_enabled(false);
    reset_cpu(PROG_BASE);
    uint8_t ram_before[0x100];
    memory_debug_read_block(0x2000, ram_before, sizeof(ram_before));
    static const uint32_t counts[] = {1, 2, 3, 5, 7, 11, 13, 40, 79, 80};
    for (size_t ci = 0; ci < sizeof(counts) / sizeof(counts[0]); ci++) {
        uint32_t n = counts[ci];
        // Reference: the switch core, in one sprint.
        predecode_set_enabled(false);
        reset_cpu(PROG_BASE);
        run(n, 1000);
        uint8_t ref[CPU_POD_SIZE];
        memcpy(ref, CPU, CPU_POD_SIZE);
        uint8_t ref_ram[0x100], ref_stack[0x100];
        memory_debug_read_block(0x2000, ref_ram, sizeof(ref_ram));
        memory_debug_read_block(0x2F00, ref_stack, sizeof(ref_stack));
        for (int level = 0; level <= 2; level++) {
            static const uint32_t budgets[] = {1, 2, 3, 4, 5, 8, 1000};
            for (size_t bi = 0; bi < sizeof(budgets) / sizeof(budgets[0]); bi++) {
                predecode_set_elide(level);
                predecode_set_enabled(true);
                predecode_reset();
                reset_cpu(PROG_BASE);
                run(n, budgets[bi]);
                if (memcmp(ref, CPU, CPU_POD_SIZE) != 0) {
                    fprintf(stderr, "cpu_t differs: n=%u level=%d budget=%u\n", n, level, budgets[bi]);
                    const uint32_t *a = (const uint32_t *)ref, *b = (const uint32_t *)CPU;
                    for (size_t i = 0; i < CPU_POD_SIZE / 4; i++)
                        if (a[i] != b[i])
                            fprintf(stderr, "  word %zu (byte %zu): ref %08X pd %08X\n", i, i * 4, a[i], b[i]);
                    ASSERT_TRUE(!"predecoded cpu_t differs from the switch core");
                }
                uint8_t got[0x100];
                memory_debug_read_block(0x2000, got, sizeof(got));
                ASSERT_TRUE(memcmp(ref_ram, got, sizeof(got)) == 0);
                memory_debug_read_block(0x2F00, got, sizeof(got));
                ASSERT_TRUE(memcmp(ref_stack, got, sizeof(got)) == 0);
                predecode_set_enabled(false);
            }
        }
    }
    predecode_set_elide(0);
    predecode_set_enabled(false);
}

// A store through the guest path into cached code takes effect at the next
// execution (invalidation), including when the program patches itself.
TEST(test_self_modifying) {
    predecode_set_enabled(true);
    predecode_reset();
    emit(0x4000, 4, 0x7001, 0x4E71, 0x4E71, 0x60F8); // MOVEQ #1,D0; NOP; NOP; BRA.S $4000
    reset_cpu(0x4000);
    run(20, 20);
    ASSERT_EQ_INT(1, (int)cpu_get_dn(CPU, 0));
    uint64_t inv0 = g_pd_stats.invalidations;
    emit(0x4000, 1, 0x7002); // patch: MOVEQ #2,D0
    ASSERT_TRUE(g_pd_stats.invalidations > inv0);
    run(20, 20);
    ASSERT_EQ_INT(2, (int)cpu_get_dn(CPU, 0));

    // The program patches its own first instruction, then jumps back to it.
    emit(0x4100, 1, 0x7001); // MOVEQ #1,D0
    emit(0x4102, 4, 0x33FC, 0x7002, 0x0000, 0x4100); // MOVE.W #$7002,$4100.L
    emit(0x410A, 3, 0x4EF9, 0x0000, 0x4100); // JMP $4100.L
    reset_cpu(0x4100);
    run(6, 6);
    ASSERT_EQ_INT(2, (int)cpu_get_dn(CPU, 0));
    ASSERT_TRUE(g_mem_code_write_count > 0);
    predecode_set_enabled(false);
}

// The look-back rule: a store resets the entries covering it and the
// eleven before, not the twelfth.
TEST(test_invalidate_lookback) {
    predecode_set_enabled(true);
    predecode_reset();
    // 32 NOPs then a self-branch at $5040.
    for (uint32_t i = 0; i < 32; i++)
        emit(0x5000 + 2 * i, 1, 0x4E71);
    emit(0x5040, 1, 0x60FE);
    reset_cpu(0x5000);
    run(40, 40);
    pd_block_t *blk = block_of(5);
    ASSERT_TRUE(blk != NULL);
    for (uint32_t i = 0; i < 33; i++)
        ASSERT_TRUE(blk->e[i].id != PD_UNDECODED);
    emit(0x5030, 1, 0x4E71); // word index 24: rewrite (same value)
    ASSERT_EQ_INT(PD_UNDECODED, blk->e[24].id);
    ASSERT_EQ_INT(PD_UNDECODED, blk->e[24 - 11].id);
    ASSERT_TRUE(blk->e[24 - 12].id != PD_UNDECODED);
    ASSERT_TRUE(blk->e[25].id != PD_UNDECODED);
    predecode_set_enabled(false);
}

// A host-side writer (DMA, debug poke) that reports through
// memory_host_written is honoured too.
TEST(test_host_written) {
    predecode_set_enabled(true);
    predecode_reset();
    emit(0x6000, 2, 0x7003, 0x60FC); // MOVEQ #3,D0; BRA.S $6000
    reset_cpu(0x6000);
    run(10, 10);
    ASSERT_EQ_INT(3, (int)cpu_get_dn(CPU, 0));
    uint8_t *host = (uint8_t *)(g_supervisor_read[6] + 0x6000);
    uint8_t patch[2] = {0x70, 0x04}; // MOVEQ #4,D0
    memory_host_written(host, 2);
    memcpy(host, patch, 2);
    run(10, 10);
    ASSERT_EQ_INT(4, (int)cpu_get_dn(CPU, 0));
    // The debug poke path reports on its own.
    ASSERT_TRUE(memory_debug_write_uint16(0x6000, 0x7005));
    run(10, 10);
    ASSERT_EQ_INT(5, (int)cpu_get_dn(CPU, 0));
    predecode_set_enabled(false);
}

// An instruction straddling the page boundary is a generic (T2) entry and
// executes correctly through the memory system.
TEST(test_cross_page) {
    predecode_set_enabled(true);
    predecode_reset();
    emit(0x1FFC, 3, 0x203C, 0x1122, 0x3344); // MOVE.L #$11223344,D0 (ext words in the next page)
    emit(0x2002, 2, 0x4E71, 0x60FE); // NOP; BRA.S self
    reset_cpu(0x1FFC);
    run(2, 2);
    ASSERT_EQ_INT((int)0x11223344, (int)cpu_get_dn(CPU, 0));
    ASSERT_EQ_INT(0x2004, (int)cpu_get_pc(CPU));
    // The entry is generic: PD_CROSS, or the leaf's T1 id (whose body
    // fetches its extension words through memory exactly as the switch
    // core does) — never a specialized id, which would need the words.
    pd_block_t *blk = block_of(1);
    ASSERT_TRUE(blk != NULL);
    uint16_t id = blk->e[(0x1FFC & PAGE_MASK) >> 1].id;
    ASSERT_TRUE(id != PD_UNDECODED && id < T1_END);
    predecode_set_enabled(false);
}

// Entering an instruction's middle decodes from that word (a jump into an
// immediate that happens to be two NOPs).
TEST(test_mid_instruction_entry) {
    predecode_set_enabled(true);
    predecode_reset();
    emit(0x5200, 3, 0x203C, 0x4E71, 0x4E71); // MOVE.L #$4E714E71,D0
    emit(0x5206, 2, 0x7207, 0x60FE); // MOVEQ #7,D1; BRA.S self
    reset_cpu(0x5200);
    run(1, 1); // the MOVE decodes at $5200
    ASSERT_EQ_INT((int)0x4E714E71, (int)cpu_get_dn(CPU, 0));
    cpu_set_pc(CPU, 0x5202); // jump into the immediate
    run(3, 3);
    ASSERT_EQ_INT(7, (int)cpu_get_dn(CPU, 1));
    ASSERT_EQ_INT(0x5208, (int)cpu_get_pc(CPU));
    predecode_set_enabled(false);
}

// A logpointed page never gets a block; it does once the logpoint is gone.
TEST(test_logpoint_page) {
    predecode_set_enabled(true);
    predecode_reset();
    emit(0x8000, 2, 0x7009, 0x60FC); // MOVEQ #9,D0; BRA.S self
    memory_logpoint_install(8, 8);
    reset_cpu(0x8000);
    uint64_t allocs0 = g_pd_stats.allocs;
    run(10, 10);
    ASSERT_EQ_INT(9, (int)cpu_get_dn(CPU, 0));
    ASSERT_TRUE(g_pd_stats.allocs == allocs0);
    memory_logpoint_uninstall(8, 8);
    run(10, 10);
    ASSERT_TRUE(g_pd_stats.allocs == allocs0 + 1);
    predecode_set_enabled(false);
}

// Pool pressure: more code pages than blocks evicts round-robin and the
// program still computes the right answer.
TEST(test_eviction) {
    predecode_set_enabled(true);
    predecode_set_pool_cap(4);
    predecode_reset();
    // Eight pages: MOVEQ #k,D0; ADD.L D0,D1; JMP next; the last parks.
    for (uint32_t k = 0; k < 8; k++) {
        uint32_t base = 0x10000 + k * 0x1000;
        emit(base, 2, 0x7000 | (k + 1), 0xD280);
        if (k < 7)
            emit(base + 4, 3, 0x4EF9, (uint16_t)((base + 0x1000) >> 16), (uint16_t)(base + 0x1000));
        else
            emit(base + 4, 1, 0x60FE);
    }
    reset_cpu(0x10000);
    run(30, 30);
    ASSERT_EQ_INT(36, (int)cpu_get_dn(CPU, 1));
    ASSERT_TRUE(g_pd_stats.evictions >= 4);
    ASSERT_TRUE(g_pd_blocks_live <= 4);
    predecode_set_pool_cap(2048);
    predecode_set_enabled(false);
}

// Thrash demotion: a page that keeps storing into itself is released to the
// generic tier after the limit and keeps computing correctly.
TEST(test_demotion) {
    predecode_set_enabled(true);
    predecode_set_thrash(4, 100000, 2);
    predecode_reset();
    emit(0x7000, 2, 0x7000, 0x5280); // MOVEQ #0,D0; loop: ADDQ.L #1,D0
    emit(0x7004, 3, 0x33C0, 0x0000, 0x7800); // MOVE.W D0,$7800.L (same page)
    emit(0x700A, 2, 0x0C40, 0x0014); // CMPI.W #20,D0
    emit(0x700E, 2, 0x66F2, 0x60FE); // BNE.S loop; BRA.S self
    reset_cpu(0x7000);
    run(120, 10);
    ASSERT_EQ_INT(20, (int)cpu_get_dn(CPU, 0));
    ASSERT_EQ_INT(20, memory_read_uint16(0x7800));
    ASSERT_TRUE(g_pd_stats.demotions >= 1);
    predecode_set_thrash(32, 4096, 65536);
    predecode_set_enabled(false);
}

// A one-word instruction in a page's last slot falls through to the next
// page: the sentinel entry past the last slot hands over to the next block
// (an entry decoded there would index the raw shadow out of bounds).
TEST(test_page_end) {
    predecode_set_enabled(true);
    predecode_reset();
    emit(0x1FFC, 2, 0x4E71, 0x4E71); // NOP; NOP (the last word of page 1)
    emit(0x2000, 2, 0x7005, 0x60FE); // MOVEQ #5,D0; BRA.S self
    reset_cpu(0x1FFC);
    run(3, 3);
    ASSERT_EQ_INT(5, (int)cpu_get_dn(CPU, 0));
    ASSERT_EQ_INT(0x2002, (int)cpu_get_pc(CPU));
    pd_block_t *blk = block_of(1);
    ASSERT_TRUE(blk != NULL);
    ASSERT_EQ_INT(PD_PAGE_END, blk->e[PD_ENTRIES_68K].id);
    ASSERT_TRUE(block_of(2) != NULL);
    predecode_set_enabled(false);
}

// A new memory map (checkpoint restore, machine boot) invalidates the pool.
TEST(test_generation_reset) {
    predecode_set_enabled(true);
    predecode_reset();
    emit(0x9000, 2, 0x7001, 0x60FC);
    reset_cpu(0x9000);
    run(4, 4);
    ASSERT_TRUE(g_pd_blocks_live >= 1);
    g_mem_map_generation++; // what memory_map_init does
    run(4, 4);
    ASSERT_EQ_INT(1, (int)g_pd_stats.allocs); // reset, then one fresh block
    predecode_set_enabled(false);
}

// === Benchmark (PD_BENCH=1: not a test; prints interpreter-loop MIPS) =======

#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// The program's inner loop runs forever with a large counter: a compact
// mix of ALU, memory, compare/branch and DBRA — the study's alu/mem kernels.
static void bench(void) {
    emit(0x1000, 5, 0x7000, 0x223C, 0x7FFF, 0xFFFF, 0x2079); // MOVEQ #0,D0; MOVE.L #$7FFFFFFF,D1; MOVEA.L $2000.L,A0
    emit(0x100A, 2, 0x0000, 0x2000); //   (the abs address)
    emit(0x100E, 2, 0xD081, 0x30C1); // loop: ADD.L D1,D0; MOVE.W D1,(A0)+
    emit(0x1012, 3, 0x0680, 0x0000, 0x0003); // ADDI.L #3,D0
    emit(0x1018, 3, 0xB081, 0x6602, 0x4E71); // CMP.L D1,D0; BNE.S skip; NOP
    emit(0x101E, 4, 0x4A80, 0x5388, 0x2208, 0x60EC); // skip: TST.L D0; SUBQ.L #1,A0; MOVE.L A0,D1; BRA.S loop
    emit(0x2000, 2, 0x0000, 0x3000);
    for (int pd = 0; pd <= 1; pd++) {
        for (int level = 0; level <= (pd ? 2 : 0); level++) {
            predecode_set_enabled(pd != 0);
            predecode_set_elide(level);
            predecode_reset();
            reset_cpu(0x1000);
            const uint32_t n = 50000000;
            double t0 = now_s();
            run(n, 100000);
            double dt = now_s() - t0;
            printf("bench: predecode=%d elide=%d  %.1f MIPS  (%u instructions in %.3f s)\n", pd, level, n / dt / 1e6, n,
                   dt);
        }
    }
    predecode_set_enabled(false);
    predecode_set_elide(0);
}

// === Main ===================================================================

int main(void) {
    CTX = test_harness_init();
    if (!CTX) {
        fprintf(stderr, "harness init failed\n");
        return 1;
    }
    CPU = test_get_cpu(CTX);
    if (getenv("PD_BENCH")) {
        bench();
        return 0;
    }
    RUN(test_basic_loop);
    RUN(test_differential);
    RUN(test_self_modifying);
    RUN(test_invalidate_lookback);
    RUN(test_host_written);
    RUN(test_cross_page);
    RUN(test_mid_instruction_entry);
    RUN(test_logpoint_page);
    RUN(test_eviction);
    RUN(test_demotion);
    RUN(test_page_end);
    RUN(test_generation_reset);
    test_harness_destroy(CTX);
    printf("[PASS] All cpu_predecode tests passed\n");
    return 0;
}
