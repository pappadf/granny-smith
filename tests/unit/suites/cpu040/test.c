// MC68040 CPU model tests (Quadra proposal Phase A gate).
//
// Hand-written cases covering the 040-specific decoder surface:
//   - MOVE16 (all five forms): line alignment, post-increment commit,
//     the (Ax)+,(Ay)+ register-pair form, same-register behavior
//   - MOVEC: the 040 control-register set (TC/ITT/DTT/URP/SRP/MMUSR),
//     write masks, rejection of the 030-only CAAR, privilege gating
//   - CINV/CPUSH: privileged functional no-ops
//   - PFLUSH/PTEST 040 forms (PTEST loads MMUSR)
//   - the 030 coprocessor MMU interface (PMOVE) F-line traps on the 040
//   - RTE with the format $7 access-error frame
//   - FSAVE/FRESTORE 040 single-longword frames (NULL/IDLE)
//   - format $2 frames for TRAPV-class exceptions
//
// Uses the cpu harness (real memory + CPU) like cpu_ea_full; the harness
// boots a 68000, so each test upgrades the model to the 68040 first.

#include "cpu.h"
#include "cpu_internal.h"
#include "fpu.h"
#include "harness.h"
#include "memory.h"
#include "mmu040.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CODE_ADDR 0x001000u
#define DATA_SRC  0x002000u
#define DATA_DST  0x002800u
#define STACK_TOP 0x003800u

void cpu_run_68040(cpu_t *cpu, uint32_t *instructions);

// Run `n` instructions on the 68040 decoder.
static void run_n(cpu_t *cpu, uint32_t n) {
    cpu_run_68040(cpu, &n);
}

// Upgrade the harness CPU (booted as a 68000) to a 68040: swap the model,
// allocate the FPU and the CPU-owned 040 MMU state the decoder expects.
static void make_68040(cpu_t *cpu) {
    cpu->cpu_model = CPU_MODEL_68040;
    if (!cpu->fpu)
        cpu->fpu = fpu_init();
    if (!cpu->mmu)
        cpu->mmu = mmu040_init();
}

static void write_words(uint32_t addr, const uint16_t *words, int n) {
    for (int i = 0; i < n; i++)
        memory_write_uint16(addr + (uint32_t)i * 2, words[i]);
}

// Reset CPU execution state to supervisor mode with a known stack.
static void reset_cpu_state(cpu_t *cpu) {
    cpu->supervisor = 1;
    cpu->m = 0;
    cpu->trace = 0;
    cpu->interrupt_mask = 7;
    cpu->ipl = 0;
    cpu->stopped = 0;
    cpu->halted = 0;
    cpu->vbr = 0;
    cpu->a[7] = STACK_TOP;
    cpu->ssp = STACK_TOP;
    cpu->pc = CODE_ADDR;
    cpu->last_bus_error_pc = 0;
}

// Fill the 16-byte line at `addr` with a recognizable pattern.
static void fill_line(uint32_t addr, uint8_t seed) {
    for (uint32_t i = 0; i < 16; i++)
        memory_write_uint8(addr + i, (uint8_t)(seed + i));
}

// Check the 16-byte line at `addr` matches the pattern from `seed`.
static int line_matches(uint32_t addr, uint8_t seed) {
    for (uint32_t i = 0; i < 16; i++)
        if (memory_read_uint8(addr + i) != (uint8_t)(seed + i))
            return 0;
    return 1;
}

// ============================================================================
// MOVE16
// ============================================================================

TEST(move16_postinc_to_abs_aligns_and_increments) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    fill_line(DATA_SRC, 0x10);
    // MOVE16 (A0)+,(xxx).L with A0 misaligned inside the line: the transfer
    // aligns to the 16-byte boundary; A0 still advances by exactly 16.
    cpu->a[0] = DATA_SRC + 7;
    uint16_t code[] = {0xF600, (uint16_t)(DATA_DST >> 16), (uint16_t)(DATA_DST & 0xFFFF)};
    write_words(CODE_ADDR, code, 3);
    run_n(cpu, 1);
    ASSERT_TRUE(line_matches(DATA_DST, 0x10));
    ASSERT_EQ_INT((int)cpu->a[0], (int)(DATA_SRC + 7 + 16));
    ASSERT_EQ_INT((int)cpu->pc, (int)(CODE_ADDR + 6));
}

TEST(move16_abs_to_postinc) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    fill_line(DATA_SRC, 0x30);
    cpu->a[1] = DATA_DST;
    uint16_t code[] = {(uint16_t)(0xF608 | 1), (uint16_t)(DATA_SRC >> 16), (uint16_t)(DATA_SRC & 0xFFFF)};
    write_words(CODE_ADDR, code, 3);
    run_n(cpu, 1);
    ASSERT_TRUE(line_matches(DATA_DST, 0x30));
    ASSERT_EQ_INT((int)cpu->a[1], (int)(DATA_DST + 16));
}

TEST(move16_an_to_abs_no_increment) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    fill_line(DATA_SRC, 0x50);
    cpu->a[2] = DATA_SRC;
    uint16_t code[] = {(uint16_t)(0xF610 | 2), (uint16_t)(DATA_DST >> 16), (uint16_t)(DATA_DST & 0xFFFF)};
    write_words(CODE_ADDR, code, 3);
    run_n(cpu, 1);
    ASSERT_TRUE(line_matches(DATA_DST, 0x50));
    ASSERT_EQ_INT((int)cpu->a[2], (int)DATA_SRC); // (An) form: no increment
}

TEST(move16_abs_to_an_no_increment) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    fill_line(DATA_SRC, 0x70);
    cpu->a[3] = DATA_DST;
    uint16_t code[] = {(uint16_t)(0xF618 | 3), (uint16_t)(DATA_SRC >> 16), (uint16_t)(DATA_SRC & 0xFFFF)};
    write_words(CODE_ADDR, code, 3);
    run_n(cpu, 1);
    ASSERT_TRUE(line_matches(DATA_DST, 0x70));
    ASSERT_EQ_INT((int)cpu->a[3], (int)DATA_DST);
}

TEST(move16_pair_postinc) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    fill_line(DATA_SRC, 0x90);
    cpu->a[0] = DATA_SRC;
    cpu->a[1] = DATA_DST + 3; // misaligned: line aligns, increment still +16
    // MOVE16 (A0)+,(A1)+: opcode $F620|Ax, ext = $8000 | Ay<<12
    uint16_t code[] = {0xF620, (uint16_t)(0x8000 | (1 << 12))};
    write_words(CODE_ADDR, code, 2);
    run_n(cpu, 1);
    ASSERT_TRUE(line_matches(DATA_DST, 0x90));
    ASSERT_EQ_INT((int)cpu->a[0], (int)(DATA_SRC + 16));
    ASSERT_EQ_INT((int)cpu->a[1], (int)(DATA_DST + 3 + 16));
    ASSERT_EQ_INT((int)cpu->pc, (int)(CODE_ADDR + 4));
}

TEST(move16_pair_same_register_double_increment) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    fill_line(DATA_SRC, 0xA0);
    cpu->a[4] = DATA_SRC;
    // MOVE16 (A4)+,(A4)+: both post-increments apply (net +32)
    uint16_t code[] = {(uint16_t)(0xF620 | 4), (uint16_t)(0x8000 | (4 << 12))};
    write_words(CODE_ADDR, code, 2);
    run_n(cpu, 1);
    ASSERT_TRUE(line_matches(DATA_SRC, 0xA0)); // copy onto itself
    ASSERT_EQ_INT((int)cpu->a[4], (int)(DATA_SRC + 32));
}

// ============================================================================
// MOVEC: 040 control-register set
// ============================================================================

// Execute MOVEC Dn,Rc then MOVEC Rc,Dm and return what reads back.
static uint32_t movec_roundtrip(cpu_t *cpu, uint16_t rc, uint32_t value) {
    reset_cpu_state(cpu);
    cpu->d[0] = value;
    uint16_t code[] = {
        0x4E7B, (uint16_t)(0x0000 | rc), // MOVEC D0,Rc
        0x4E7A, (uint16_t)(0x1000 | rc), // MOVEC Rc,D1
    };
    write_words(CODE_ADDR, code, 4);
    cpu->d[1] = 0xDEADBEEF;
    run_n(cpu, 2);
    return cpu->d[1];
}

TEST(movec_040_register_set_roundtrip) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    // TC: only E (bit 15) and P (bit 14) are implemented
    ASSERT_EQ_INT((int)movec_roundtrip(cpu, 0x003, 0xFFFFFFFF), (int)0x0000C000);
    // TTRs: writable mask $FFFFE364
    ASSERT_EQ_INT((int)movec_roundtrip(cpu, 0x004, 0xFFFFFFFF), (int)0xFFFFE364);
    ASSERT_EQ_INT((int)movec_roundtrip(cpu, 0x005, 0x12348421), (int)(0x12348421 & 0xFFFFE364));
    ASSERT_EQ_INT((int)movec_roundtrip(cpu, 0x006, 0xFF00A365), (int)(0xFF00A365 & 0xFFFFE364));
    ASSERT_EQ_INT((int)movec_roundtrip(cpu, 0x007, 0x00FF6064), (int)(0x00FF6064 & 0xFFFFE364));
    // URP/SRP: 512-byte aligned (low 9 bits read back zero)
    ASSERT_EQ_INT((int)movec_roundtrip(cpu, 0x806, 0x123455FF), (int)0x12345400);
    ASSERT_EQ_INT((int)movec_roundtrip(cpu, 0x807, 0x87654321), (int)(0x87654321 & 0xFFFFFE00));
    // MMUSR: full 32 bits
    ASSERT_EQ_INT((int)movec_roundtrip(cpu, 0x805, 0xCAFEF00D), (int)0xCAFEF00D);
    // CACR: 040 layout — only DE (bit 31) and IE (bit 15)
    ASSERT_EQ_INT((int)movec_roundtrip(cpu, 0x002, 0xFFFFFFFF), (int)0x80008000);
    // VBR: full 32 bits (restore 0 afterwards for later tests)
    ASSERT_EQ_INT((int)movec_roundtrip(cpu, 0x801, 0x00008000), (int)0x00008000);
    cpu->vbr = 0;
}

TEST(movec_tc_enable_bit_tracks_mmu_state) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    mmu040_state_t *mmu = (mmu040_state_t *)cpu->mmu;
    movec_roundtrip(cpu, 0x003, 0x8000);
    ASSERT_TRUE(mmu->enabled);
    movec_roundtrip(cpu, 0x003, 0x0000);
    ASSERT_TRUE(!mmu->enabled);
}

TEST(movec_caar_illegal_on_040) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    memory_write_uint32(0x010, 0x004000); // vector 4: illegal instruction
    uint16_t code[] = {0x4E7A, 0x0802}; // MOVEC CAAR,D0 — 030-only
    write_words(CODE_ADDR, code, 2);
    run_n(cpu, 1);
    ASSERT_EQ_INT((int)cpu->pc, 0x004000);
    // Stacked PC points at the MOVEC itself (instruction_pc)
    ASSERT_EQ_INT((int)memory_read_uint32(cpu->a[7] + 2), (int)CODE_ADDR);
}

TEST(movec_privileged_from_user_mode) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    memory_write_uint32(0x020, 0x004100); // vector 8: privilege violation
    uint16_t code[] = {0x4E7A, 0x1801}; // MOVEC VBR,D1
    write_words(CODE_ADDR, code, 2);
    cpu->usp = STACK_TOP - 0x100;
    cpu_set_sr(cpu, 0x0000); // drop to user mode
    cpu->pc = CODE_ADDR;
    run_n(cpu, 1);
    ASSERT_EQ_INT((int)cpu->pc, 0x004100);
    ASSERT_TRUE(cpu->supervisor);
}

// ============================================================================
// CINV / CPUSH
// ============================================================================

TEST(cinv_cpush_execute_in_supervisor) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    cpu->a[0] = DATA_SRC;
    uint16_t code[] = {
        0xF4D8, // CINVA BC   ($F400 | 3<<6 | 3<<3)
        0xF4C8, // CINVL BC,(A0)  ($F400 | 3<<6 | 1<<3)
        0xF4F8, // CPUSHA BC  ($F400 | 3<<6 | 7<<3)
        0xF470, // CPUSHP DC,(A0) ($F400 | 1<<6 | 6<<3)
    };
    write_words(CODE_ADDR, code, 4);
    run_n(cpu, 4);
    // All four execute as functional no-ops and fall through
    ASSERT_EQ_INT((int)cpu->pc, (int)(CODE_ADDR + 8));
    ASSERT_TRUE(cpu->supervisor);
}

TEST(cinv_privileged_from_user_mode) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    memory_write_uint32(0x020, 0x004200); // vector 8: privilege violation
    uint16_t code[] = {0xF4D8}; // CINVA BC
    write_words(CODE_ADDR, code, 1);
    cpu->usp = STACK_TOP - 0x100;
    cpu_set_sr(cpu, 0x0000);
    cpu->pc = CODE_ADDR;
    run_n(cpu, 1);
    ASSERT_EQ_INT((int)cpu->pc, 0x004200);
    ASSERT_TRUE(cpu->supervisor);
}

// ============================================================================
// PFLUSH / PTEST (040 forms)
// ============================================================================

TEST(pflush_forms_execute) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    cpu->a[0] = DATA_SRC;
    uint16_t code[] = {
        0xF500, // PFLUSHN (A0)
        0xF508, // PFLUSH (A0)
        0xF510, // PFLUSHAN
        0xF518, // PFLUSHA
    };
    write_words(CODE_ADDR, code, 4);
    run_n(cpu, 4);
    ASSERT_EQ_INT((int)cpu->pc, (int)(CODE_ADDR + 8));
}

TEST(ptest_disabled_reports_resident_identity) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    mmu040_state_t *mmu = (mmu040_state_t *)cpu->mmu;
    mmu->enabled = false;
    mmu->mmusr = 0xFFFFFFFF;
    cpu->a[0] = 0x00345678;
    uint16_t code[] = {0xF548}; // PTESTW (A0)
    write_words(CODE_ADDR, code, 1);
    run_n(cpu, 1);
    ASSERT_EQ_INT((int)mmu->mmusr, (int)((0x00345678 & 0xFFFFF000) | MMUSR040_R));
}

// ============================================================================
// 030 coprocessor MMU interface is gone on the 040
// ============================================================================

TEST(pmove_fline_traps_on_040) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    memory_write_uint32(0x02C, 0x004300); // vector 11: line F
    uint16_t code[] = {0xF010, 0x4000}; // PMOVE TC,(A0) on a 030
    write_words(CODE_ADDR, code, 2);
    run_n(cpu, 1);
    ASSERT_EQ_INT((int)cpu->pc, 0x004300);
}

// ============================================================================
// Exception frames
// ============================================================================

TEST(rte_format7_pops_access_error_frame) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    // Hand-build a format $7 frame (30 words = 60 bytes) at the stack top
    uint32_t frame = STACK_TOP - 60;
    for (uint32_t i = 0; i < 60; i += 4)
        memory_write_uint32(frame + i, 0);
    memory_write_uint16(frame + 0x00, 0x2000); // SR: supervisor
    memory_write_uint32(frame + 0x02, 0x00404000 & 0x00FFFFFF); // resume PC
    memory_write_uint16(frame + 0x06, 0x7008); // format $7, vector 2
    cpu->a[7] = frame;
    uint16_t code[] = {0x4E73}; // RTE
    write_words(CODE_ADDR, code, 1);
    run_n(cpu, 1);
    ASSERT_EQ_INT((int)cpu->pc, (int)(0x00404000 & 0x00FFFFFF));
    ASSERT_EQ_INT((int)cpu->a[7], (int)STACK_TOP); // whole 60-byte frame popped
    ASSERT_TRUE(cpu->supervisor);
}

TEST(trapv_uses_format2_frame) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    memory_write_uint32(0x01C, 0x004400); // vector 7: TRAPV
    uint16_t code[] = {0x4E76}; // TRAPV
    write_words(CODE_ADDR, code, 1);
    cpu->overflow = 1;
    run_n(cpu, 1);
    ASSERT_EQ_INT((int)cpu->pc, 0x004400);
    // Frame: SR(2) PC(4) fmt/vec(2) instr-addr(4) — format $2, vector offset $1C
    uint16_t fmtvec = memory_read_uint16(cpu->a[7] + 6);
    ASSERT_EQ_INT((fmtvec >> 12) & 0xF, 0x2);
    ASSERT_EQ_INT(fmtvec & 0xFFF, 0x01C);
    ASSERT_EQ_INT((int)memory_read_uint32(cpu->a[7] + 8), (int)CODE_ADDR);
}

// ============================================================================
// FSAVE / FRESTORE (040 frame formats)
// ============================================================================

TEST(fsave_null_and_idle_frames) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    fpu_state_t *fpu = (fpu_state_t *)cpu->fpu;

    // Reset-state FPU: FSAVE -(A7) writes a NULL frame (one longword of 0)
    fpu->initialized = false;
    uint16_t code[] = {0xF327}; // FSAVE -(A7)
    write_words(CODE_ADDR, code, 1);
    run_n(cpu, 1);
    ASSERT_EQ_INT((int)cpu->a[7], (int)(STACK_TOP - 4));
    ASSERT_EQ_INT((int)memory_read_uint32(cpu->a[7]), 0);

    // Initialized FPU: FSAVE writes the IDLE frame (version $41, size $00)
    fpu->initialized = true;
    cpu->pc = CODE_ADDR;
    cpu->a[7] = STACK_TOP;
    run_n(cpu, 1);
    ASSERT_EQ_INT((int)cpu->a[7], (int)(STACK_TOP - 4));
    ASSERT_EQ_INT((int)memory_read_uint32(cpu->a[7]), (int)0x41000000);
    // FSAVE leaves the FPU in the null state
    ASSERT_TRUE(!fpu->initialized);
}

TEST(frestore_null_resets_and_idle_rearms) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    fpu_state_t *fpu = (fpu_state_t *)cpu->fpu;

    // FRESTORE (A7)+ of an IDLE frame re-arms the FPU and pops 4 bytes
    memory_write_uint32(STACK_TOP - 4, 0x41000000);
    cpu->a[7] = STACK_TOP - 4;
    fpu->initialized = false;
    uint16_t code[] = {0xF35F}; // FRESTORE (A7)+
    write_words(CODE_ADDR, code, 1);
    run_n(cpu, 1);
    ASSERT_TRUE(fpu->initialized);
    ASSERT_EQ_INT((int)cpu->a[7], (int)STACK_TOP);

    // FRESTORE of a NULL frame resets control registers
    fpu->initialized = true;
    fpu->fpcr = 0x0000FF00;
    memory_write_uint32(STACK_TOP - 4, 0x00000000);
    cpu->a[7] = STACK_TOP - 4;
    cpu->pc = CODE_ADDR;
    run_n(cpu, 1);
    ASSERT_TRUE(!fpu->initialized);
    ASSERT_EQ_INT((int)fpu->fpcr, 0);
    ASSERT_EQ_INT((int)cpu->a[7], (int)STACK_TOP);
}

// ============================================================================
// Integer sanity on the 040 decoder
// ============================================================================

TEST(integer_ops_run_on_040_decoder) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68040(cpu);
    reset_cpu_state(cpu);
    uint16_t code[] = {
        0x7005, // MOVEQ #5,D0
        0xD07C, 0x0003, // ADD.W #3,D0
        0x2400, // MOVE.L D0,D2
    };
    write_words(CODE_ADDR, code, 4);
    run_n(cpu, 3);
    ASSERT_EQ_INT((int)cpu->d[2], 8);
}

int main(void) {
    test_context_t *ctx = test_harness_init();
    if (!ctx) {
        fprintf(stderr, "harness init failed\n");
        return 1;
    }

    RUN(move16_postinc_to_abs_aligns_and_increments);
    RUN(move16_abs_to_postinc);
    RUN(move16_an_to_abs_no_increment);
    RUN(move16_abs_to_an_no_increment);
    RUN(move16_pair_postinc);
    RUN(move16_pair_same_register_double_increment);
    RUN(movec_040_register_set_roundtrip);
    RUN(movec_tc_enable_bit_tracks_mmu_state);
    RUN(movec_caar_illegal_on_040);
    RUN(movec_privileged_from_user_mode);
    RUN(cinv_cpush_execute_in_supervisor);
    RUN(cinv_privileged_from_user_mode);
    RUN(pflush_forms_execute);
    RUN(ptest_disabled_reports_resident_identity);
    RUN(pmove_fline_traps_on_040);
    RUN(rte_format7_pops_access_error_frame);
    RUN(trapv_uses_format2_frame);
    RUN(fsave_null_and_idle_frames);
    RUN(frestore_null_resets_and_idle_rearms);
    RUN(integer_ops_run_on_040_decoder);

    test_harness_destroy(ctx);
    printf("[cpu040] all tests passed\n");
    return 0;
}
