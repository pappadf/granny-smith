// FMOVEM.X (FPU data register list) direction and ordering tests.
//
// Validates fpu_movem_data() (src/core/cpu/fpu.c) by executing single
// 68030 FMOVEM.X instructions and checking which way data moved.
//
// The dr bit (extension word bit 13) selects the transfer direction:
// dr=1 is FPU→memory (save), dr=0 is memory→FPU (restore). Compilers
// emit stack forms (-(An)/(An)+), where the direction is implied by the
// EA mode — but A/UX cc emits frame-based control-mode forms
// (FMOVEM.X FP2/FP3,d16(A6)), which must honour the dr bit. A dr
// inversion in the control-mode path went unnoticed by all Mac-side
// software and broke every A/UX userland FP callee-save (see the
// 2026-07-19 iifx-aux3-x11 xclock note): the "restore" leaked libm's
// scratch FP3 into xclock's sinangle.
//
// The -(An) form additionally uses a reversed register list
// (bit i = FPi) versus bit 7 = FP0 everywhere else.

#include "cpu.h"
#include "cpu_internal.h"
#include "fpu.h"
#include "harness.h"
#include "memory.h"
#include "test_assert.h"

#include <stdint.h>
#include <string.h>

#define CODE_ADDR 0x001000u
#define DATA_ADDR 0x002000u

// Run one 68030 instruction starting at cpu->pc.
static void run_one(cpu_t *cpu) {
    extern void cpu_run_68030(cpu_t * cpu, uint32_t * instructions);
    uint32_t one = 1;
    cpu_run_68030(cpu, &one);
}

static void make_68030(cpu_t *cpu) {
    cpu->cpu_model = CPU_MODEL_68030;
    if (!cpu->fpu)
        cpu->fpu = fpu_init();
}

static fpu_state_t *get_fpu(cpu_t *cpu) {
    return (fpu_state_t *)cpu->fpu;
}

// FMOVEM.X opcode: 1111 001 000 mmm rrr (F-line, CpID=1, type=0)
static uint16_t fmovem_opcode(int ea_mode, int ea_reg) {
    return (uint16_t)(0xF200 | (ea_mode << 3) | ea_reg);
}

// Write a 12-byte extended-precision image of an easy value: sign/exp
// word from fp80_make-style fields, mantissa 64 bits, 16 bits of zero
// padding between them (68882 in-memory extended format).
static void write_ext96(uint32_t addr, uint16_t sign_exp, uint64_t mant) {
    memory_write_uint32(addr, (uint32_t)sign_exp << 16);
    memory_write_uint32(addr + 4, (uint32_t)(mant >> 32));
    memory_write_uint32(addr + 8, (uint32_t)mant);
}

// Small pool of distinct, exactly-representable values: 1.0, 2.0, 3.0, ...
static float80_reg_t val(int n) {
    // n must be 1..15: exponent chosen so mantissa is exact
    // 1.0 = exp 0x3FFF mant 0x8000...; 2.0 = 0x4000/0x8000...; 3.0 = 0x4000/0xC000...
    switch (n) {
    case 1:
        return fp80_make(0, 0x3FFF, 0x8000000000000000ULL);
    case 2:
        return fp80_make(0, 0x4000, 0x8000000000000000ULL);
    case 3:
        return fp80_make(0, 0x4000, 0xC000000000000000ULL);
    case 4:
        return fp80_make(0, 0x4001, 0x8000000000000000ULL);
    default:
        return fp80_make(0, 0x4001, 0x8000000000000000ULL | ((uint64_t)n << 32));
    }
}

static bool fp80_eq(float80_reg_t a, float80_reg_t b) {
    return a.exponent == b.exponent && a.mantissa == b.mantissa;
}

// Read back a 12-byte extended image as a float80_reg_t.
static float80_reg_t read_ext96(uint32_t addr) {
    float80_reg_t r;
    r.exponent = (uint16_t)(memory_read_uint32(addr) >> 16);
    r.mantissa = ((uint64_t)memory_read_uint32(addr + 4) << 32) | memory_read_uint32(addr + 8);
    return r;
}

// Fill the data area with a sentinel pattern.
static void fill_sentinel(uint32_t addr, int bytes) {
    for (int i = 0; i < bytes; i += 4)
        memory_write_uint32(addr + i, 0xCAFED00D);
}

// Execute FMOVEM.X with the given opcode/ext (plus optional extra word).
static void exec_fmovem(cpu_t *cpu, uint16_t opcode, uint16_t ext, const uint16_t *extra, int n_extra) {
    memory_write_uint16(CODE_ADDR, opcode);
    memory_write_uint16(CODE_ADDR + 2, ext);
    for (int i = 0; i < n_extra; i++)
        memory_write_uint16(CODE_ADDR + 4 + i * 2, extra[i]);
    cpu->pc = CODE_ADDR;
    run_one(cpu);
}

// --- control mode d16(An): the A/UX compiler frame-save form ---------------

TEST(fmovem_control_d16_save) {
    // FMOVEM.X FP2/FP3,$100(A6)   ext = 111 1 0 000 | 0x30 = 0xF030
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    fpu_state_t *fpu = get_fpu(cpu);

    fpu->fp[2] = val(2);
    fpu->fp[3] = val(3);
    fill_sentinel(DATA_ADDR + 0x100, 24);
    cpu->a[6] = DATA_ADDR;
    uint16_t disp[] = {0x0100};
    exec_fmovem(cpu, fmovem_opcode(5, 6), 0xF030, disp, 1);

    // Registers unchanged, memory holds FP2 then FP3 (ascending)
    ASSERT_TRUE(fp80_eq(fpu->fp[2], val(2)));
    ASSERT_TRUE(fp80_eq(fpu->fp[3], val(3)));
    ASSERT_TRUE(fp80_eq(read_ext96(DATA_ADDR + 0x100), val(2)));
    ASSERT_TRUE(fp80_eq(read_ext96(DATA_ADDR + 0x100 + 12), val(3)));
}

TEST(fmovem_control_d16_restore) {
    // FMOVEM.X $100(A6),FP2/FP3   ext = 110 1 0 000 | 0x30 = 0xD030
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    fpu_state_t *fpu = get_fpu(cpu);

    write_ext96(DATA_ADDR + 0x100, val(2).exponent, val(2).mantissa);
    write_ext96(DATA_ADDR + 0x100 + 12, val(3).exponent, val(3).mantissa);
    fpu->fp[2] = val(9); // sentinels
    fpu->fp[3] = val(10);
    cpu->a[6] = DATA_ADDR;
    uint16_t disp[] = {0x0100};
    exec_fmovem(cpu, fmovem_opcode(5, 6), 0xD030, disp, 1);

    // Registers loaded from memory; memory unchanged
    ASSERT_TRUE(fp80_eq(fpu->fp[2], val(2)));
    ASSERT_TRUE(fp80_eq(fpu->fp[3], val(3)));
    ASSERT_TRUE(fp80_eq(read_ext96(DATA_ADDR + 0x100), val(2)));
}

TEST(fmovem_control_d16_save_restore_roundtrip) {
    // Save FP2/FP3, clobber them, restore: the callee-save pattern the
    // A/UX libm trig kernel uses (F030 prologue / D030 epilogue).
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    fpu_state_t *fpu = get_fpu(cpu);

    fpu->fp[2] = val(4);
    fpu->fp[3] = val(5);
    cpu->a[6] = DATA_ADDR;
    uint16_t disp[] = {0x0040};
    exec_fmovem(cpu, fmovem_opcode(5, 6), 0xF030, disp, 1); // save

    fpu->fp[2] = val(11); // callee scratch
    fpu->fp[3] = val(12);
    exec_fmovem(cpu, fmovem_opcode(5, 6), 0xD030, disp, 1); // restore

    ASSERT_TRUE(fp80_eq(fpu->fp[2], val(4)));
    ASSERT_TRUE(fp80_eq(fpu->fp[3], val(5)));
}

// --- (An) indirect: the A/UX kernel context-switch form ---------------------

TEST(fmovem_an_indirect_save_all_restore_all) {
    // FMOVEM.X FP0-FP7,(A0) then FMOVEM.X (A0),FP0-FP7
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    fpu_state_t *fpu = get_fpu(cpu);

    for (int i = 0; i < 8; i++)
        fpu->fp[i] = val(i + 1);
    cpu->a[0] = DATA_ADDR;
    exec_fmovem(cpu, fmovem_opcode(2, 0), 0xF0FF, NULL, 0); // save all

    for (int i = 0; i < 8; i++) {
        ASSERT_TRUE(fp80_eq(read_ext96(DATA_ADDR + i * 12), val(i + 1)));
        fpu->fp[i] = val(15); // clobber
    }
    exec_fmovem(cpu, fmovem_opcode(2, 0), 0xD0FF, NULL, 0); // restore all
    for (int i = 0; i < 8; i++)
        ASSERT_TRUE(fp80_eq(fpu->fp[i], val(i + 1)));
    ASSERT_EQ_INT((int)cpu->a[0], (int)DATA_ADDR); // (An): no increment
}

// --- stack forms: -(An) save / (An)+ restore ---------------------------------

TEST(fmovem_predec_save_postinc_restore) {
    // FMOVEM.X FP2/FP3,-(A7): ext = 111 0 0 000 | reversed list (bit i = FPi)
    // FP2/FP3 -> bits 2,3 = 0x0C -> ext 0xE00C
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    fpu_state_t *fpu = get_fpu(cpu);

    fpu->fp[2] = val(6);
    fpu->fp[3] = val(7);
    cpu->a[7] = DATA_ADDR + 0x200;
    exec_fmovem(cpu, fmovem_opcode(4, 7), 0xE00C, NULL, 0);

    // A7 dropped by 24; memory: FP2 at lower address, FP3 above
    ASSERT_EQ_INT((int)cpu->a[7], (int)(DATA_ADDR + 0x200 - 24));
    ASSERT_TRUE(fp80_eq(read_ext96(DATA_ADDR + 0x200 - 24), val(6)));
    ASSERT_TRUE(fp80_eq(read_ext96(DATA_ADDR + 0x200 - 12), val(7)));

    // FMOVEM.X (A7)+,FP2/FP3: ext = 110 1 0 000 | 0x30 = 0xD030
    fpu->fp[2] = val(13);
    fpu->fp[3] = val(14);
    exec_fmovem(cpu, fmovem_opcode(3, 7), 0xD030, NULL, 0);
    ASSERT_EQ_INT((int)cpu->a[7], (int)(DATA_ADDR + 0x200));
    ASSERT_TRUE(fp80_eq(fpu->fp[2], val(6)));
    ASSERT_TRUE(fp80_eq(fpu->fp[3], val(7)));
}

// --- dynamic register list (Dn) ----------------------------------------------

TEST(fmovem_control_dynamic_list_save) {
    // FMOVEM.X D1,$40(A6) (dynamic list in D1) ext = 111 1 1 001 000 0000
    //   = 0xF000 | 0x800 | (1 << 4) = 0xF810; D1 holds the static-format mask
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    fpu_state_t *fpu = get_fpu(cpu);

    fpu->fp[1] = val(8);
    cpu->d[1] = 0x40; // bit 6 = FP1
    cpu->a[6] = DATA_ADDR;
    fill_sentinel(DATA_ADDR + 0x40, 12);
    uint16_t disp[] = {0x0040};
    exec_fmovem(cpu, fmovem_opcode(5, 6), 0xF810, disp, 1);
    ASSERT_TRUE(fp80_eq(read_ext96(DATA_ADDR + 0x40), val(8)));
}

int main(void) {
    test_context_t *ctx = test_harness_init();
    if (!ctx) {
        fprintf(stderr, "Failed to initialize test harness\n");
        return 1;
    }

    RUN(fmovem_control_d16_save);
    RUN(fmovem_control_d16_restore);
    RUN(fmovem_control_d16_save_restore_roundtrip);
    RUN(fmovem_an_indirect_save_all_restore_all);
    RUN(fmovem_predec_save_postinc_restore);
    RUN(fmovem_control_dynamic_list_save);

    test_harness_destroy(ctx);
    return 0;
}
