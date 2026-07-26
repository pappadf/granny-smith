// 68020+ bit-field instruction conformance.
//
// The single-step conformance vectors in suites/cpu are 68000 vectors, so the
// bit-field instructions — which only exist on 68020 and later — have no
// coverage at all, despite Mac OS software leaning on them heavily (a Mac OS
// 7.6 Installer run executes over ten million of them: the Apple installer's
// decompressor uses BFEXTU as its bit reader).
//
// Each case is checked against an independent reference model that treats
// memory as a flat MSB-first bit string, per M68000 PRM §"Bit Field
// Instructions": the field is the `width` bits starting `offset` bits after
// the effective address, where offset is signed and may be arbitrarily large.

#include "cpu.h"
#include "cpu_internal.h"
#include "harness.h"
#include "memory.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CODE_ADDR 0x001000u
#define DATA_ADDR 0x002000u
#define DATA_LEN  64

static uint8_t g_ref[DATA_LEN]; // reference copy of the operand bytes

static void run_one(cpu_t *cpu) {
    extern void cpu_run_68030(cpu_t * cpu, uint32_t * instructions);
    uint32_t one = 1;
    cpu_run_68030(cpu, &one);
}

// The harness builds a 68000; the bit-field opcodes only decode on the 030.
static void make_68030(cpu_t *cpu) {
    extern void *fpu_init(void);
    cpu->cpu_model = CPU_MODEL_68030;
    if (!cpu->fpu)
        cpu->fpu = fpu_init();
}

// Reference: read bit `bit` (MSB-first) of the operand region, where bit 0 is
// the most significant bit of the byte at DATA_ADDR.
static int ref_bit(int32_t bit) {
    int32_t byte = bit >> 3; // arithmetic shift: floor division for negatives
    uint32_t idx = (uint32_t)(byte + (DATA_ADDR - DATA_ADDR));
    if (byte < 0 || (uint32_t)byte >= DATA_LEN)
        return -1;
    return (g_ref[idx] >> (7 - (bit & 7))) & 1;
}

// Reference: extract `width` bits starting at `offset` bits past DATA_ADDR.
static uint32_t ref_extract(int32_t offset, uint32_t width) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < width; i++) {
        int b = ref_bit(offset + (int32_t)i);
        v = (v << 1) | (uint32_t)(b < 0 ? 0 : b);
    }
    return v;
}

// Reference: insert `value`'s low `width` bits at `offset`.
static void ref_insert(int32_t offset, uint32_t width, uint32_t value) {
    for (uint32_t i = 0; i < width; i++) {
        int32_t bit = offset + (int32_t)i;
        int32_t byte = bit >> 3;
        if (byte < 0 || (uint32_t)byte >= DATA_LEN)
            continue;
        uint32_t v = (value >> (width - 1 - i)) & 1u;
        uint8_t m = (uint8_t)(1u << (7 - (bit & 7)));
        g_ref[byte] = (uint8_t)((g_ref[byte] & ~m) | (v ? m : 0));
    }
}

// Deterministic pseudo-random operand bytes.
static void seed_operand(uint32_t seed) {
    uint32_t x = seed * 2654435761u + 1u;
    for (int i = 0; i < DATA_LEN; i++) {
        x = x * 1103515245u + 12345u;
        g_ref[i] = (uint8_t)(x >> 16);
        memory_write_uint8(DATA_ADDR + i, g_ref[i]);
    }
}

// Build the bit-field extension word: Do/offset, Dw/width, and Dn register.
static uint16_t bf_ext(int off_is_reg, uint32_t off, int w_is_reg, uint32_t w, uint32_t dn) {
    uint16_t e = (uint16_t)((dn & 7u) << 12);
    e |= (uint16_t)(off_is_reg ? (0x0800 | ((off & 7u) << 6)) : ((off & 31u) << 6));
    e |= (uint16_t)(w_is_reg ? (0x0020 | (w & 7u)) : (w & 31u));
    return e;
}

// BFEXTU <ea>{offset:width},Dn — opcode $E9C0 | mode<<3 | reg
static uint32_t exec_bfextu_mem(cpu_t *cpu, int32_t offset, uint32_t width) {
    memory_write_uint16(CODE_ADDR, (uint16_t)(0xE9C0 | (2 << 3) | 0)); // (A0)
    memory_write_uint16(CODE_ADDR + 2, bf_ext(1, 1, 1, 2, 3)); // off=D1 width=D2 -> D3
    cpu->pc = CODE_ADDR;
    cpu->a[0] = DATA_ADDR;
    cpu->d[1] = (uint32_t)offset;
    cpu->d[2] = width & 31u; // width 0 in the register means 32
    cpu->d[3] = 0xDEADBEEFu;
    run_one(cpu);
    return cpu->d[3];
}

// BFINS Dn,<ea>{offset:width} — opcode $EFC0 | mode<<3 | reg
static void exec_bfins_mem(cpu_t *cpu, int32_t offset, uint32_t width, uint32_t value) {
    memory_write_uint16(CODE_ADDR, (uint16_t)(0xEFC0 | (2 << 3) | 0));
    memory_write_uint16(CODE_ADDR + 2, bf_ext(1, 1, 1, 2, 3));
    cpu->pc = CODE_ADDR;
    cpu->a[0] = DATA_ADDR;
    cpu->d[1] = (uint32_t)offset;
    cpu->d[2] = width & 31u;
    cpu->d[3] = value;
    run_one(cpu);
}

TEST(bfextu_mem_matches_reference) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    int checked = 0;
    for (uint32_t seed = 1; seed <= 8; seed++) {
        seed_operand(seed);
        for (int32_t off = 0; off <= 71; off++) {
            for (uint32_t w = 1; w <= 32; w++) {
                uint32_t got = exec_bfextu_mem(cpu, off, w);
                uint32_t want = ref_extract(off, w);
                if (got != want) {
                    printf("BFEXTU (A0){%d:%u} seed %u: got $%08X want $%08X\n", off, w, seed, got, want);
                    ASSERT_EQ_INT((int)got, (int)want);
                }
                checked++;
            }
        }
    }
    printf("[bitfield] BFEXTU memory: %d cases\n", checked);
}

TEST(bfextu_mem_negative_offset) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    seed_operand(42);
    // A negative offset reaches bits *before* the effective address; point the
    // EA into the middle of the operand so those bytes are still seeded.
    for (int32_t off = -64; off < 0; off++) {
        for (uint32_t w = 1; w <= 32; w++) {
            memory_write_uint16(CODE_ADDR, (uint16_t)(0xE9C0 | (2 << 3) | 0));
            memory_write_uint16(CODE_ADDR + 2, bf_ext(1, 1, 1, 2, 3));
            cpu->pc = CODE_ADDR;
            cpu->a[0] = DATA_ADDR + 32; // 32 bytes in
            cpu->d[1] = (uint32_t)off;
            cpu->d[2] = w & 31u;
            cpu->d[3] = 0;
            run_one(cpu);
            uint32_t want = ref_extract(off + 32 * 8, w);
            if (cpu->d[3] != want) {
                printf("BFEXTU (A0+32){%d:%u}: got $%08X want $%08X\n", off, w, cpu->d[3], want);
                ASSERT_EQ_INT((int)cpu->d[3], (int)want);
            }
        }
    }
}

TEST(bfins_mem_matches_reference) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    int checked = 0;
    for (uint32_t seed = 1; seed <= 4; seed++) {
        for (int32_t off = 0; off <= 39; off++) {
            for (uint32_t w = 1; w <= 32; w++) {
                seed_operand(seed);
                uint32_t value = 0x89ABCDEFu ^ (uint32_t)(off * 2654435761u) ^ w;
                exec_bfins_mem(cpu, off, w, value);
                ref_insert(off, w, value);
                for (int i = 0; i < DATA_LEN; i++) {
                    uint8_t got = memory_read_uint8(DATA_ADDR + i);
                    if (got != g_ref[i]) {
                        printf("BFINS (A0){%d:%u} byte %d: got $%02X want $%02X\n", off, w, i, got, g_ref[i]);
                        ASSERT_EQ_INT((int)got, (int)g_ref[i]);
                    }
                }
                checked++;
            }
        }
    }
    printf("[bitfield] BFINS memory: %d cases\n", checked);
}

TEST(bfextu_flags) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    seed_operand(7);
    // N is the most significant bit of the extracted field, Z is set when the
    // field is zero, and V and C are always cleared (PRM).
    for (int32_t off = 0; off <= 39; off++) {
        for (uint32_t w = 1; w <= 32; w++) {
            uint32_t got = exec_bfextu_mem(cpu, off, w);
            uint32_t want = ref_extract(off, w);
            ASSERT_EQ_INT((int)got, (int)want);
            int want_n = (int)((want >> (w - 1)) & 1u);
            ASSERT_EQ_INT((int)cpu->negative, want_n);
            ASSERT_EQ_INT((int)cpu->zero, (int)(want == 0));
            ASSERT_EQ_INT((int)cpu->overflow, 0);
            ASSERT_EQ_INT((int)cpu->carry, 0);
        }
    }
}

// --- the remaining six bit-field opcodes ---------------------------------
// BFTST $E8C0, BFEXTU $E9C0, BFCHG $EAC0, BFEXTS $EBC0,
// BFCLR $ECC0, BFFFO $EDC0, BFSET $EEC0, BFINS $EFC0

// Run one bit-field op against (A0) with register-supplied offset/width.
static void exec_bf_mem(cpu_t *cpu, uint16_t base_op, int32_t offset, uint32_t width, uint32_t dn_in) {
    memory_write_uint16(CODE_ADDR, (uint16_t)(base_op | (2 << 3) | 0));
    memory_write_uint16(CODE_ADDR + 2, bf_ext(1, 1, 1, 2, 3));
    cpu->pc = CODE_ADDR;
    cpu->a[0] = DATA_ADDR;
    cpu->d[1] = (uint32_t)offset;
    cpu->d[2] = width & 31u;
    cpu->d[3] = dn_in;
    run_one(cpu);
}

TEST(bfexts_sign_extends) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    seed_operand(11);
    for (int32_t off = 0; off <= 39; off++) {
        for (uint32_t w = 1; w <= 32; w++) {
            exec_bf_mem(cpu, 0xEBC0, off, w, 0);
            uint32_t raw = ref_extract(off, w);
            uint32_t want = (w < 32u) ? (uint32_t)((int32_t)(raw << (32u - w)) >> (32u - w)) : raw;
            if (cpu->d[3] != want) {
                printf("BFEXTS (A0){%d:%u}: got $%08X want $%08X\n", off, w, cpu->d[3], want);
                ASSERT_EQ_INT((int)cpu->d[3], (int)want);
            }
        }
    }
}

TEST(bfffo_finds_first_one) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    seed_operand(13);
    for (int32_t off = 0; off <= 39; off++) {
        for (uint32_t w = 1; w <= 32; w++) {
            exec_bf_mem(cpu, 0xEDC0, off, w, 0);
            uint32_t f = ref_extract(off, w);
            uint32_t pos = w;
            for (uint32_t i = 0; i < w; i++) {
                if ((f >> (w - 1u - i)) & 1u) {
                    pos = i;
                    break;
                }
            }
            uint32_t want = (uint32_t)off + pos;
            if (cpu->d[3] != want) {
                printf("BFFFO (A0){%d:%u} field $%08X: got $%08X want $%08X\n", off, w, f, cpu->d[3], want);
                ASSERT_EQ_INT((int)cpu->d[3], (int)want);
            }
        }
    }
}

// BFTST must not modify memory; BFSET/BFCLR/BFCHG rewrite the field.
TEST(bftst_set_clr_chg_memory) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    struct {
        uint16_t op;
        const char *name;
        int mode;
    } cases[] = {
        {0xE8C0, "BFTST", 0},
        {0xEEC0, "BFSET", 1},
        {0xECC0, "BFCLR", 2},
        {0xEAC0, "BFCHG", 3},
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        for (int32_t off = 0; off <= 39; off++) {
            for (uint32_t w = 1; w <= 32; w++) {
                seed_operand(23 + c);
                uint32_t before = ref_extract(off, w);
                exec_bf_mem(cpu, cases[c].op, off, w, 0);
                uint32_t want_field = before;
                if (cases[c].mode == 1)
                    want_field = (w == 32u) ? 0xFFFFFFFFu : ((1u << w) - 1u);
                else if (cases[c].mode == 2)
                    want_field = 0;
                else if (cases[c].mode == 3)
                    want_field = (w == 32u) ? ~before : ((~before) & ((1u << w) - 1u));
                if (cases[c].mode != 0)
                    ref_insert(off, w, want_field);
                for (int i = 0; i < DATA_LEN; i++) {
                    uint8_t got = memory_read_uint8(DATA_ADDR + i);
                    if (got != g_ref[i]) {
                        printf("%s (A0){%d:%u} byte %d: got $%02X want $%02X\n", cases[c].name, off, w, i, got,
                               g_ref[i]);
                        ASSERT_EQ_INT((int)got, (int)g_ref[i]);
                    }
                }
                // Flags always reflect the field as it was BEFORE the update.
                ASSERT_EQ_INT((int)cpu->zero, (int)(before == 0));
                ASSERT_EQ_INT((int)cpu->negative, (int)((before >> (w - 1)) & 1u));
            }
        }
    }
}

// A bit reader walking a large buffer produces offsets far beyond one byte;
// these are the values the Mac OS installer's decompressor actually uses.
TEST(bfextu_large_offsets) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    seed_operand(31);
    for (int32_t base = 0; base < 8 * (DATA_LEN - 8); base += 3) {
        for (uint32_t w = 1; w <= 32; w += 7) {
            uint32_t got = exec_bfextu_mem(cpu, base, w);
            uint32_t want = ref_extract(base, w);
            if (got != want) {
                printf("BFEXTU (A0){%d:%u}: got $%08X want $%08X\n", base, w, got, want);
                ASSERT_EQ_INT((int)got, (int)want);
            }
        }
    }
}

// The immediate offset/width encodings (no Dn) take a different decode path.
TEST(bfextu_immediate_offset_width) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    seed_operand(37);
    for (uint32_t off = 0; off < 32; off++) {
        for (uint32_t w = 0; w < 32; w++) {
            memory_write_uint16(CODE_ADDR, (uint16_t)(0xE9C0 | (2 << 3) | 0));
            memory_write_uint16(CODE_ADDR + 2, bf_ext(0, off, 0, w, 3));
            cpu->pc = CODE_ADDR;
            cpu->a[0] = DATA_ADDR;
            cpu->d[3] = 0;
            run_one(cpu);
            uint32_t eff_w = (w == 0) ? 32u : w; // width 0 encodes 32
            uint32_t want = ref_extract((int32_t)off, eff_w);
            if (cpu->d[3] != want) {
                printf("BFEXTU imm (A0){%u:%u}: got $%08X want $%08X\n", off, eff_w, cpu->d[3], want);
                ASSERT_EQ_INT((int)cpu->d[3], (int)want);
            }
        }
    }
}

// Bit-field operands may use any control addressing mode; the decompressor
// reaches its buffer through a displacement, so cover those paths too.
TEST(bfextu_other_ea_modes) {
    cpu_t *cpu = test_get_cpu(test_get_active_context());
    make_68030(cpu);
    seed_operand(53);
    for (int32_t off = 0; off <= 39; off++) {
        for (uint32_t w = 1; w <= 32; w += 3) {
            uint32_t want = ref_extract(off + 16 * 8, w); // field 16 bytes in

            // (d16,A0) with A0 = DATA_ADDR, d16 = 16
            memory_write_uint16(CODE_ADDR, (uint16_t)(0xE9C0 | (5 << 3) | 0));
            memory_write_uint16(CODE_ADDR + 2, bf_ext(1, 1, 1, 2, 3));
            memory_write_uint16(CODE_ADDR + 4, 16);
            cpu->pc = CODE_ADDR;
            cpu->a[0] = DATA_ADDR;
            cpu->d[1] = (uint32_t)off;
            cpu->d[2] = w & 31u;
            cpu->d[3] = 0;
            run_one(cpu);
            if (cpu->d[3] != want) {
                printf("BFEXTU 16(A0){%d:%u}: got $%08X want $%08X\n", off, w, cpu->d[3], want);
                ASSERT_EQ_INT((int)cpu->d[3], (int)want);
            }

            // (A0,D0.W) with A0 = DATA_ADDR, D0.W = 16 (brief extension word)
            memory_write_uint16(CODE_ADDR, (uint16_t)(0xE9C0 | (6 << 3) | 0));
            memory_write_uint16(CODE_ADDR + 2, bf_ext(1, 1, 1, 2, 3));
            memory_write_uint16(CODE_ADDR + 4, 0x0000); // D0.W, scale 1, disp 0
            cpu->pc = CODE_ADDR;
            cpu->a[0] = DATA_ADDR;
            cpu->d[0] = 16;
            cpu->d[1] = (uint32_t)off;
            cpu->d[2] = w & 31u;
            cpu->d[3] = 0;
            run_one(cpu);
            if (cpu->d[3] != want) {
                printf("BFEXTU (A0,D0.W){%d:%u}: got $%08X want $%08X\n", off, w, cpu->d[3], want);
                ASSERT_EQ_INT((int)cpu->d[3], (int)want);
            }

            // absolute long
            memory_write_uint16(CODE_ADDR, (uint16_t)(0xE9C0 | (7 << 3) | 1));
            memory_write_uint16(CODE_ADDR + 2, bf_ext(1, 1, 1, 2, 3));
            memory_write_uint32(CODE_ADDR + 4, DATA_ADDR + 16);
            cpu->pc = CODE_ADDR;
            cpu->d[1] = (uint32_t)off;
            cpu->d[2] = w & 31u;
            cpu->d[3] = 0;
            run_one(cpu);
            if (cpu->d[3] != want) {
                printf("BFEXTU $abs{%d:%u}: got $%08X want $%08X\n", off, w, cpu->d[3], want);
                ASSERT_EQ_INT((int)cpu->d[3], (int)want);
            }
        }
    }
}

int main(void) {
    test_context_t *ctx = test_harness_init();
    if (!ctx) {
        fprintf(stderr, "Failed to initialize test harness\n");
        return 1;
    }

    RUN(bfextu_mem_matches_reference);
    RUN(bfextu_mem_negative_offset);
    RUN(bfins_mem_matches_reference);
    RUN(bfextu_flags);
    RUN(bfexts_sign_extends);
    RUN(bfffo_finds_first_one);
    RUN(bftst_set_clr_chg_memory);
    RUN(bfextu_large_offsets);
    RUN(bfextu_immediate_offset_width);
    RUN(bfextu_other_ea_modes);

    test_harness_destroy(ctx);
    return 0;
}
