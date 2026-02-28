// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_disasm.c
// Motorola 68000 instruction disassembler for debugging output.

#include "cpu.h"
#include "debug_mac.h"

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint16_t disasm_fetch_16_without_inc(uint16_t *fetch_pos) {
    uint16_t v = fetch_pos[1];

    return v;
}

static uint32_t disasm_fetch_32_without_inc(uint16_t *fetch_pos) {
    uint32_t v = fetch_pos[1] << 16 | fetch_pos[2];

    return v;
}

static uint16_t disasm_fetch_16(uint16_t **fetch_pos, int offset) {
    *fetch_pos += 1 + offset;

    uint16_t v = **fetch_pos;

    (*fetch_pos)++;

    return v;
}

static uint32_t disasm_fetch_32(uint16_t **fetch_pos, int offset) {
    *fetch_pos += 1 + offset;

    uint16_t v1 = **fetch_pos;

    (*fetch_pos)++;

    uint16_t v2 = **fetch_pos;

    (*fetch_pos)++;

    return (uint32_t)v1 << 16 | v2;
}

static const char *an[] = {"A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7"};
static const char *dn[] = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7"};
static const char *cc[] = {"T",  "F",  "HI", "LS", "CC", "CS", "NE", "EQ",
                           "VC", "VS", "PL", "MI", "GE", "LT", "GT", "LE"};
static const char *bcc[] = {"RA", "?",  "HI", "LS", "CC", "CS", "NE", "EQ",
                            "VC", "VS", "PL", "MI", "GE", "LT", "GT", "LE"};

static const char *format_pc_displacement(int32_t disp) {
    static char buf[20];

    if (disp >= 0)
        sprintf(buf, "*+$%04X", (int)(disp));
    else
        sprintf(buf, "*-$%04X", (int)(0 - disp));

    return buf;
}

static const char *tmp_buf_printf(const char *fmt, ...) {
    enum { N = 160 };
    static char b[4][N]; // small ring to survive nested calls
    static unsigned idx;
    char *s = b[idx++ & 3];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, N, fmt, ap);
    va_end(ap);
    return s;
}

static const char *disasm_ea(int size, int mode, int reg, uint16_t **fetch_pos, int ext_words,
                             ea_mode_t supported_modes) {
    const char *buf = "<illegal>";
    if (!((supported_modes) & 1u << (mode + (mode == 7 ? reg : 0))))
        return buf;

    uint16_t *pos = *fetch_pos;

    pos += ext_words + 1;

    switch (mode) {
    case 0: // Dn
        buf = tmp_buf_printf("D%d", (int)(reg));
        break;
    case 1: // An
        buf = tmp_buf_printf("A%d", (int)(reg));
        break;
    case 2: // (An)
        buf = tmp_buf_printf("(A%d)", (int)(reg));
        break;
    case 3: // (An)+
        buf = tmp_buf_printf("(A%d)+", (int)(reg));
        break;
    case 4: // -(An)
        buf = tmp_buf_printf("-(A%d)", (int)(reg));
        break;
    case 5: // (d16,An)
        buf = tmp_buf_printf("%s$%X(A%d)", (int16_t)*pos >= 0 ? "" : "-",
                             (int16_t)*pos >= 0 ? (int)(int16_t)*pos : 0 - (int16_t)*pos, (int)(reg));
        pos += 1;
        break;
    case 6: // (d8,An,Xn)
        if (*pos & 0x0800)
            buf = tmp_buf_printf("%s$%X(%s,%s.L*%d)", (int8_t)*pos >= 0 ? "" : "-",
                                 (int8_t)*pos >= 0 ? (int)(int8_t)*pos : 0 - (int8_t)*pos, an[reg],
                                 *pos & 0x8000 ? an[*pos >> 12 & 7] : dn[*pos >> 12 & 7], 1 << (*pos >> 9 & 3));
        else
            buf = tmp_buf_printf("%s$%X(%s,%s.W*%d)", (int8_t)*pos >= 0 ? "" : "-",
                                 (int8_t)*pos >= 0 ? (int)(int8_t)*pos : 0 - (int8_t)*pos, an[reg],
                                 *pos & 0x8000 ? an[*pos >> 12 & 7] : dn[*pos >> 12 & 7], 1 << (*pos >> 9 & 3));
        pos++;
        break;
    case 0x7:
        switch (reg) {
        case 0: // (xxx).W
            buf = tmp_buf_printf("$%04X", (int)(int16_t)*pos);
            pos++;
            break;
        case 1: // (xxx).L
            buf = tmp_buf_printf("$%08X", (int)((uint32_t)pos[0] << 16 | pos[1]));
            pos += 2;
            break;
        case 2: // (d16,PC)
            buf = tmp_buf_printf("*%s$%X", (int16_t)*pos >= 0 ? "+" : "-",
                                 (int16_t)*pos >= 0 ? (int)(int16_t)*pos + 2 + 2 * ext_words : 0 - (int16_t)*pos);
            pos++;
            break;
        case 3: // (d8,PC,Xn)
            if (*pos & 0x0800)
                buf = tmp_buf_printf("*%s$%04X(%s.L*%d)", (int8_t)*pos >= 0 ? "+" : "-",
                                     (int8_t)*pos >= 0 ? (int)(int8_t)*pos + 2 + 2 * ext_words : 0 - (int8_t)*pos,
                                     *pos & 0x8000 ? an[*pos >> 12 & 7] : dn[*pos >> 12 & 7], 1 << (*pos >> 9 & 3));
            else
                buf = tmp_buf_printf("*%s$%04X(%s.W*%d)", (int8_t)*pos >= 0 ? "+" : "-",
                                     (int8_t)*pos >= 0 ? (int)(int8_t)*pos + 2 + 2 * ext_words : 0 - (int8_t)*pos,
                                     *pos & 0x8000 ? an[*pos >> 12 & 7] : dn[*pos >> 12 & 7], 1 << (*pos >> 9 & 3));
            pos++;
            break;
        case 4: // #data
            if (size == 4) {
                buf = tmp_buf_printf("#$%X", (int)((uint32_t)(pos[0]) << 16 | pos[1]));
                pos += 2;
            } else if (size == 2) {
                buf = tmp_buf_printf("#$%X", (int)pos[0]);
                pos++;
            } else if (size == 1) {
                buf = tmp_buf_printf("#$%X", (int)(pos[0]));
                pos++;
            } else
                assert(0);
            break;
        }
    }

    // Ensure we always advance the caller's fetch position
    *fetch_pos = pos;

    return buf;
}

static int ea_words(int mode, int reg, int size) {
    switch (mode) {
    case 0: // Dn
    case 1: // An
    case 2: // (An)
    case 3: // (An)+
    case 4: // -(An)
        return 0;
    case 5: // (d16,An)
    case 6: // (d8,An,Xn)
        return 1;
    case 0x7:
        switch (reg) {
        case 0: // (xxx).W
            return 1;
        case 1: // (xxx).L
            return 2;
        case 2: // (d16,PC)
            return 1;
        case 3: // (d8,PC,Xn)
            return 1;
        case 4: // #data
            return size > 2 ? 2 : 1;
        }
    }

    return 0;
}

static char *asm_movem(uint16_t opcode, uint16_t mask) {

    // for the predecrement mode, register mask is reversed
    if ((opcode >> 3 & 7) == 4)
        mask = reverse16(mask);

    int i, j;

    static char s[100] = ""; // 100 bytes should be plenty
    int n = 100;

    for (i = 0; i < 16; i++) {

        if (mask & 1 << i) {
            n -= snprintf(s + 100 - n, n, "%c%d", i > 7 ? 'A' : 'D', i & 7);
            for (j = i; j < 16; j++)
                if (~mask & 1 << j)
                    break;
            if (i < j - 1)
                n -= snprintf(s + 100 - n, n, "-%c%d", j - 1 > 7 ? 'A' : 'D', (j - 1) & 7);
            n -= snprintf(s + 100 - n, n, "/");
            i = j;
        }
    }

    if (s[strlen(s) - 1] == '/')
        s[strlen(s) - 1] = 0;

    return s;
}

static const char *movec_cr_name(uint16_t code) {
    static const char *cr_names[8] = {"SFC", "DFC", "CACR", "TC", "ITT0", "ITT1", "DTT0", "DTT1"};
    static const char *cr_names_800[8] = {"USP", "VBR", "CAAR", "MSP", "ISP", "MMUSR", "URP", "SRP"};
    if ((code & 0xFF8) == 0x000)
        return cr_names[code & 0x7];
    if ((code & 0xFF8) == 0x800)
        return cr_names_800[code & 0x7];
    return NULL;
}

static const char *disasm_pbcc(unsigned c) {
    static const char *fcond[16] = {"BS", "BC", "LS", "LC", "SS", "SC", "AS", "AC",
                                    "WS", "WC", "IS", "IC", "GS", "GC", "CS", "CC"};
    return fcond[c & 0xF];
}

static const char *disasm_fbcc(unsigned c) {
    static const char *fbcc[32] = {"F",   "EQ",  "OGT",  "OGE", "OLT", "OLE", "OGL", "OR",  "UN",   "UEQ", "UGT",
                                   "UGE", "ULT", "ULE",  "NE",  "T",   "SF",  "SEQ", "GT",  "GE",   "LT",  "LE",
                                   "GL",  "GLE", "NGLE", "NGL", "NLE", "NLT", "NGE", "NGT", "SNEQ", "ST"};

    return fbcc[c & 0x3F];
}

static const char *disasm_caches(unsigned c) {
    static const char *caches[4] = {"NC", "DC", "IC", "DC/IC"};
    return caches[c & 3];
}

static const char *disasm_cache_scope(unsigned c) {
    static const char *caches[4] = {"", "L", "P", "A"};
    return caches[c & 3];
}

#define DISASM

#define ASM(...)                                                                                                       \
    { sprintf(buf, __VA_ARGS__); }
#define INSTR(x)

#define EXT_WORD (int)disasm_fetch_16_without_inc(fetch_pos_src)

#define EXT_LONG (int)disasm_fetch_32_without_inc(fetch_pos_src)

#define SRC_WORD (int)disasm_fetch_16(&fetch_pos_src, 0)

#define SRC_LONG (int)disasm_fetch_32(&fetch_pos_src, 0)

#define DST_WORD (int)disasm_fetch_16(&fetch_pos_dst, 0)
#define DST_LONG (int)disasm_fetch_32(&fetch_pos_dst, 0)

#define DST_EA(size, offset, mode) disasm_ea(size, opcode >> 3 & 7, opcode & 7, &fetch_pos_dst, offset, mode)

#define SRC_EA(size, offset, mode) disasm_ea(size, opcode >> 3 & 7, opcode & 7, &fetch_pos_src, offset, mode)

#define AD(ad, reg) (ad ? an[reg] : dn[reg])
#define RN          AD(ext_word >> 15 & 1, ext_word >> 12 & 7)

#define RC movec_cr_name(SRC_WORD & 0x0FFF)

#define IMM ((((opcode >> 9 & 7) - 1) & 7) + 1)

#define MOVE_DST(size)                                                                                                 \
    disasm_ea(size, opcode >> 6 & 7, opcode >> 9 & 7, &fetch_pos_dst, SRC_EXT_WORDS(size), ea_alterable)

#define LIST asm_movem(opcode, ext_word)

#define CC  cc[opcode >> 8 & 0xF]
#define BCC bcc[opcode >> 8 & 0xF]

#define PBCC disasm_pbcc(opcode & 0x3F)
#define FBCC disasm_fbcc(opcode & 0x3F)

#define DX dn[opcode >> 9 & 7]
#define DY dn[opcode & 7]

#define AX an[opcode >> 9 & 7]
#define AY an[opcode & 7]

#define DN dn[opcode >> 9 & 7]
#define AN an[opcode >> 9 & 7]

#define DC dn[EXT_WORD & 7]
#define DR dn[EXT_WORD & 7]
#define DH dn[EXT_WORD & 7]
#define DU dn[EXT_WORD >> 6 & 7]

#define DC1 dn[EXT_LONG >> 16 & 7]
#define DU1 dn[EXT_LONG >> 22 & 7]
#define RN1 dn[EXT_LONG >> 28 & 7]
#define DC2 dn[EXT_LONG & 7]
#define DU2 dn[EXT_LONG >> 6 & 7]
#define RN2 dn[DST_LONG >> 12 & 7]

#define DQ dn[ext_word >> 12 & 7]
#define DL dn[ext_word >> 12 & 7]

#define SRC_EXT_WORDS(size) ea_words(opcode >> 3 & 7, opcode & 7, size)

#define SHORT_PC_DISP format_pc_displacement((int8_t)(opcode & 0xFF) + 2)
#define LONG_PC_DISP  format_pc_displacement(disasm_fetch_32(&fetch_pos_src, 0) + 2)
#define PC_DISP       format_pc_displacement((int16_t)SRC_WORD + 2)

#define CACHES disasm_caches(opcode >> 6 & 3)

#define BF_OFFSET_STR (ext_word & 0x0800 ? dn[(ext_word >> 6) & 7] : tmp_buf_printf("#$%X", (ext_word >> 6) & 31))
#define BF_WIDTH_STR                                                                                                   \
    (ext_word & 0x0020 ? dn[ext_word & 7] : tmp_buf_printf("#$%X", (ext_word & 31) ? (ext_word & 31) : 32))
#define BF_DREG_STR dn[(ext_word >> 12) & 7]

#define BF_OPERANDS(ea_mode) tmp_buf_printf("%s{%s:%s}", DST_EA(4, 1, ea_mode), BF_OFFSET_STR, BF_WIDTH_STR)
#define BF_OPERANDS_DREG(ea_mode)                                                                                      \
    tmp_buf_printf("%s{%s:%s},%s", DST_EA(4, 1, ea_mode), BF_OFFSET_STR, BF_WIDTH_STR, BF_DREG_STR)
#define BF_OPERANDS_SREG(ea_mode)                                                                                      \
    tmp_buf_printf("%s,%s{%s:%s}", BF_DREG_STR, DST_EA(4, 1, ea_mode), BF_OFFSET_STR, BF_WIDTH_STR)

#define OP_ABCD_DY_DX         ASM("ABCD\t%s,%s", DY, DX)
#define OP_ABCD_AY_AX         ASM("ABCD\t-(%s),-(%s)", AY, AX)
#define OP_ADD_B_EA_DN        ASM("ADD.B\t%s,%s", SRC_EA(1, 0, ea_any - ea_an), DN);
#define OP_ADD_W_EA_DN        ASM("ADD.W\t%s,%s", SRC_EA(2, 0, ea_any), DN);
#define OP_ADD_L_EA_DN        ASM("ADD.L\t%s,%s", SRC_EA(4, 0, ea_any), DN);
#define OP_ADD_B_DN_EA        ASM("ADD.B\t%s,%s", DN, DST_EA(1, 0, (ea_data & ea_alterable)));
#define OP_ADD_W_DN_EA        ASM("ADD.W\t%s,%s", DN, DST_EA(2, 0, (ea_data & ea_alterable)));
#define OP_ADD_L_DN_EA        ASM("ADD.L\t%s,%s", DN, DST_EA(4, 0, (ea_data & ea_alterable)));
#define OP_ADDA_L_EA_AN       ASM("ADDA.L\t%s,%s", SRC_EA(4, 0, ea_any), AN);
#define OP_ADDA_W_EA_AN       ASM("ADDA.W\t%s,%s", SRC_EA(2, 0, ea_any), AN);
#define OP_ADDI_B_DATA_EA     ASM("ADDI.B\t#$%X,%s", SRC_WORD, DST_EA(1, 1, (ea_data & ea_alterable)));
#define OP_ADDI_L_DATA_EA     ASM("ADDI.L\t#$%X,%s", SRC_LONG, DST_EA(4, 2, (ea_data & ea_alterable)));
#define OP_ADDI_W_DATA_EA     ASM("ADDI.W\t#$%X,%s", SRC_WORD, DST_EA(2, 1, (ea_data & ea_alterable)));
#define OP_ADDQ_B_DATA_EA     ASM("ADDQ.B\t#$%X,%s", (int)IMM, DST_EA(1, 0, ea_alterable & ~ea_an))
#define OP_ADDQ_L_DATA_AN     ASM("ADDQ.L\t#$%X,%s", (int)IMM, DST_EA(4, 0, ea_an))
#define OP_ADDQ_L_DATA_EA     ASM("ADDQ.L\t#$%X,%s", (int)IMM, DST_EA(4, 0, ea_alterable & ~ea_an))
#define OP_ADDQ_W_DATA_AN     ASM("ADDQ.W\t#$%X,%s", (int)IMM, DST_EA(2, 0, ea_an))
#define OP_ADDQ_W_DATA_EA     ASM("ADDQ.W\t#$%X,%s", (int)(IMM), DST_EA(2, 0, ea_alterable & ~ea_an))
#define OP_ADDX_B_AY_AX       ASM("ADDX.B\t-(%s),-(%s)", AY, AX);
#define OP_ADDX_B_DY_DX       ASM("ADDX.B\t%s,%s", DY, DX);
#define OP_ADDX_L_AY_AX       ASM("ADDX.L\t-(%s),-(%s)", AY, AX);
#define OP_ADDX_L_DY_DX       ASM("ADDX.L\t%s,%s", DY, DX);
#define OP_ADDX_W_AY_AX       ASM("ADDX.W\t-(%s),-(%s)", AY, AX);
#define OP_ADDX_W_DY_DX       ASM("ADDX.W\t%s,%s", DY, DX);
#define OP_AND_B_DN_EA        ASM("AND.B\t%s,%s", DN, DST_EA(1, 0, (ea_memory & ea_alterable)))
#define OP_AND_B_EA_DN        ASM("AND.B\t%s,%s", SRC_EA(1, 0, ea_data), DN)
#define OP_AND_L_DN_EA        ASM("AND.L\t%s,%s", DN, DST_EA(4, 0, (ea_memory & ea_alterable)))
#define OP_AND_L_EA_DN        ASM("AND.L\t%s,%s", SRC_EA(4, 0, ea_data), DN)
#define OP_AND_W_DN_EA        ASM("AND.W\t%s,%s", DN, DST_EA(2, 0, (ea_memory & ea_alterable)))
#define OP_AND_W_EA_DN        ASM("AND.W\t%s,%s", SRC_EA(2, 0, ea_data), DN)
#define OP_ANDI_B_DATA_CCR    ASM("ANDI.B\t#$%04X,CCR", SRC_WORD)
#define OP_ANDI_B_DATA_EA     ASM("ANDI.B\t#$%X,%s", SRC_WORD, DST_EA(1, 1, (ea_data & ea_alterable)))
#define OP_ANDI_L_DATA_EA     ASM("ANDI.L\t#$%08X,%s", SRC_LONG, DST_EA(4, 2, (ea_data & ea_alterable)))
#define OP_ANDI_W_DATA_EA     ASM("ANDI.W\t#$%04X,%s", SRC_WORD, DST_EA(2, 1, (ea_data & ea_alterable)))
#define OP_ANDI_W_DATA_SR     ASM("ANDI.W\t#$%04X,SR", SRC_WORD)
#define OP_ASL_B_DATA_DY      ASM("ASL.B\t#$%X,%s", (int)(IMM), DY);
#define OP_ASL_B_DX_DY        ASM("ASL.B\t%s,%s", DX, DY);
#define OP_ASL_L_DATA_DY      ASM("ASL.L\t#$%X,%s", (int)(IMM), DY);
#define OP_ASL_L_DX_DY        ASM("ASL.L\t%s,%s", DX, DY);
#define OP_ASL_W_DATA_DY      ASM("ASL.W\t#$%X,%s", (int)(IMM), DY);
#define OP_ASL_W_DX_DY        ASM("ASL.W\t%s,%s", DX, DY);
#define OP_ASL_W_EA           ASM("ASL.W\t%s", DST_EA(2, 0, (ea_memory & ea_alterable)));
#define OP_ASR_B_DATA_DY      ASM("ASR.B\t#$%X,%s", (int)(IMM), DY);
#define OP_ASR_B_DX_DY        ASM("ASR.B\t%s,%s", DX, DY);
#define OP_ASR_L_DATA_DY      ASM("ASR.L\t#$%X,%s", (int)(IMM), DY);
#define OP_ASR_L_DX_DY        ASM("ASR.L\t%s,%s", DX, DY);
#define OP_ASR_W_DATA_DY      ASM("ASR.W\t#$%X,%s", (int)(IMM), DY);
#define OP_ASR_W_DX_DY        ASM("ASR.W\t%s,%s", DX, DY);
#define OP_ASR_W_EA           ASM("ASR.W\t%s", DST_EA(2, 0, (ea_memory & ea_alterable)));
#define OP_ATRAP              ASM("%s", macos_atrap_name(opcode))
#define OP_BCC_B_DISPLACEMENT ASM("B%s.S\t%s", BCC, SHORT_PC_DISP);
#define OP_BCC_L_DISPLACEMENT ASM("B%s.L\t%s", BCC, LONG_PC_DISP);
#define OP_BCC_W_DISPLACEMENT ASM("B%s\t%s", BCC, PC_DISP);
#define OP_BCHG_B_DATA_EA     ASM("BCHG\t#$%X,%s", SRC_WORD, DST_EA(1, 1, (ea_data & ea_alterable)))
#define OP_BCHG_B_DN_EA       ASM("BCHG\t%s,%s", DN, DST_EA(1, 0, (ea_data & ea_alterable)))
#define OP_BCHG_L_DATA_DN     ASM("BCHG\t#$%X,%s", SRC_WORD, DY)
#define OP_BCHG_L_DX_DY       ASM("BCHG\t%s,%s", DX, DY)
#define OP_BCLR_B_DATA_EA     ASM("BCLR\t#$%X,%s", SRC_WORD, DST_EA(1, 1, (ea_data & ea_alterable)))
#define OP_BCLR_B_DN_EA       ASM("BCLR\t%s,%s", DN, DST_EA(1, 0, (ea_data & ea_alterable)))
#define OP_BCLR_L_DATA_DN     ASM("BCLR\t#$%X,%s", SRC_WORD, DST_EA(1, 1, (ea_data & ea_alterable))) // ?
#define OP_BCLR_L_DX_DY       ASM("BCLR\t%s,%s", DX, DY)
#define OP_BKPT_DATA          ASM("BKPT\t#$%01x", (int)(opcode & 7))
#define OP_BSET_B_DATA_EA     ASM("BSET\t#$%X,%s", SRC_WORD, DST_EA(1, 1, (ea_data & ea_alterable)))
#define OP_BSET_B_DN_EA       ASM("BSET\t%s,%s", DN, DST_EA(1, 0, (ea_data & ea_alterable)))
#define OP_BSET_L_DATA_DN     ASM("BSET\t#$%X,%s", SRC_WORD, DST_EA(1, 1, (ea_data & ea_alterable)))
#define OP_BSET_L_DX_DY       ASM("BSET\t%s,%s", DX, DY)
#define OP_BSR_B_LABEL        ASM("BSR.S\t%s", SHORT_PC_DISP);
#define OP_BSR_L_LABEL        ASM("BSR.L\t%s", LONG_PC_DISP);
#define OP_BSR_W_LABEL        ASM("BSR\t%s", PC_DISP);
#define OP_BTST_B_DATA_EA     ASM("BTST\t#$%X,%s", SRC_WORD, DST_EA(1, 1, ea_data & ~ea_xxx))
#define OP_BTST_B_DN_EA       ASM("BTST\t%s,%s", DN, DST_EA(1, 0, ea_data & ~ea_xxx))
#define OP_BTST_L_DATA_DN     ASM("BTST\t#$%X,%s", SRC_WORD, DST_EA(1, 1, ea_data & ~ea_xxx))
#define OP_BTST_L_DX_DY       ASM("BTST\t%s,%s", DX, DY)
#define OP_BFTST_EA           ASM("BFTST\t%s", BF_OPERANDS(ea_control))
#define OP_BFTST_DN           ASM("BFTST\t%s", BF_OPERANDS(ea_dn))
#define OP_BFCHG_EA           ASM("BFCHG\t%s", BF_OPERANDS((ea_control & ea_alterable)))
#define OP_BFCHG_DN           ASM("BFCHG\t%s", BF_OPERANDS(ea_dn))
#define OP_BFCLR_EA           ASM("BFCLR\t%s", BF_OPERANDS((ea_control & ea_alterable)))
#define OP_BFCLR_DN           ASM("BFCLR\t%s", BF_OPERANDS(ea_dn))
#define OP_BFSET_EA           ASM("BFSET\t%s", BF_OPERANDS((ea_control & ea_alterable)))
#define OP_BFSET_DN           ASM("BFSET\t%s", BF_OPERANDS(ea_dn))
#define OP_BFEXTS_EA          ASM("BFEXTS\t%s", BF_OPERANDS_DREG(ea_control))
#define OP_BFEXTS_DN          ASM("BFEXTS\t%s", BF_OPERANDS_DREG(ea_dn))
#define OP_BFEXTU_EA          ASM("BFEXTU\t%s", BF_OPERANDS_DREG(ea_control))
#define OP_BFEXTU_DN          ASM("BFEXTU\t%s", BF_OPERANDS_DREG(ea_dn))
#define OP_BFFFO_EA           ASM("BFFFO\t%s", BF_OPERANDS_DREG(ea_control))
#define OP_BFFFO_DN           ASM("BFFFO\t%s", BF_OPERANDS_DREG(ea_dn))
#define OP_BFINS_EA           ASM("BFINS\t%s", BF_OPERANDS_SREG((ea_control & ea_alterable)))
#define OP_BFINS_DN           ASM("BFINS\t%s", BF_OPERANDS_SREG(ea_dn))
#define OP_CALLM_DATA_EA      ASM("CALLM\t#$%X,%s", SRC_WORD, DST_EA(4, 1, ea_control))
#define OP_CAS_B_DC_DU_EA     ASM("CAS.B\t%s,%s,%s", DC, DU, DST_EA(1, 1, (ea_memory & ea_alterable)))
#define OP_CAS_L_DC_DU_EA     ASM("CAS.L\t%s,%s,%s", DC, DU, DST_EA(4, 1, (ea_memory & ea_alterable)))
#define OP_CAS_W_DC_DU_EA     ASM("CAS.W\t%s,%s,%s", DC, DU, DST_EA(2, 1, (ea_memory & ea_alterable)))
#define OP_CAS2_L_DC_DU_RN    ASM("CAS2.L\t%s:%s,%s:%s,(%s):(%s)", DC1, DC2, DU1, DU2, RN1, RN2)
#define OP_CAS2_W_DC_DU_RN    ASM("CAS2.W\t%s:%s,%s:%s,(%s):(%s)", DC1, DC2, DU1, DU2, RN1, RN2)
#define OP_CHK_L_EA_DN        ASM("CHK.L\t%s,%s", SRC_EA(4, 0, ea_any), DN)
#define OP_CHK_W_EA_DN        ASM("CHK.W\t%s,%s", SRC_EA(2, 0, ea_any), DN)
#define OP_CHK2_B_EA_DN       ASM("CHK2.B\t%s,%s", SRC_EA(1, 1, ea_any), RN)
#define OP_CHK2_L_EA_DN       ASM("CHK2.L\t%s,%s", SRC_EA(1, 1, ea_any), RN)
#define OP_CHK2_W_EA_DN       ASM("CHK2.W\t%s,%s", SRC_EA(1, 1, ea_any), RN)
#define OP_CLR_B_EA           ASM("CLR.B\t%s", DST_EA(1, 0, (ea_data & ea_alterable)))
#define OP_CLR_L_EA           ASM("CLR.L\t%s", DST_EA(4, 0, (ea_data & ea_alterable)))
#define OP_CLR_W_EA           ASM("CLR.W\t%s", DST_EA(2, 0, (ea_data & ea_alterable)))
#define OP_CMP_B_EA_DN        ASM("CMP.B\t%s,%s", SRC_EA(1, 0, ea_any & ~ea_an), DN);
#define OP_CMP_L_EA_DN        ASM("CMP.L\t%s,%s", SRC_EA(4, 0, ea_any), DN);
#define OP_CMP_W_EA_DN        ASM("CMP.W\t%s,%s", SRC_EA(2, 0, ea_any), DN);
#define OP_CMPA_W_EA_AN       ASM("CMPA.W\t%s,%s", SRC_EA(2, 0, ea_any), AN);
#define OP_CMPI_B_DATA_EA     ASM("CMPI.B\t#$%X,%s", SRC_WORD, DST_EA(1, 1, ea_data & ~ea_xxx))
#define OP_CMPI_L_DATA_EA     ASM("CMPI.L\t#$%08X,%s", SRC_LONG, DST_EA(4, 2, ea_data & ~ea_xxx))
#define OP_CMPI_W_DATA_EA     ASM("CMPI.W\t#$%X,%s", SRC_WORD, DST_EA(2, 1, ea_data & ~ea_xxx))
#define OP_CMPM_B_AY_AX       ASM("CMPM.B\t(%s)+,(%s)+", AY, AX);
#define OP_CMPM_L_AY_AX       ASM("CMPM.L\t(%s)+,(%s)+", AY, AX);
#define OP_CMPA_L_EA_AN       ASM("CMPA.L\t%s,%s", SRC_EA(4, 0, ea_any), AN);
#define OP_CMPM_W_AY_AX       ASM("CMPM.W\t(%s)+,(%s)+", AY, AX);
#define OP_DBCC_DN_LABEL      ASM("DB%s\t%s,*+$%X", CC, DY, (int)SRC_WORD + 2)
#define OP_DIVS_L_EA_DR_DQ    ASM("DIVS.L\t%s,%s:%s", SRC_EA(4, 1, ea_any), DR, DQ)
#define OP_DIVS_W_EA_DN       ASM("DIVS.W\t%s,%s", SRC_EA(2, 0, ea_data), DN)
#define OP_DIVU_W_EA_DN       ASM("DIVU.W\t%s,%s", SRC_EA(2, 0, ea_data), DN)
#define OP_EOR_B_DN_EA        ASM("EOR.B\t%s,%s", DN, DST_EA(1, 0, (ea_data & ea_alterable)));
#define OP_EOR_L_DN_EA        ASM("EOR.L\t%s,%s", DN, DST_EA(4, 0, (ea_data & ea_alterable)));
#define OP_EOR_W_DN_EA        ASM("EOR.W\t%s,%s", DN, DST_EA(2, 0, (ea_data & ea_alterable)));
#define OP_EORI_B_DATA_CCR    ASM("EORI.B\t#$%04X,CCR", SRC_WORD)
#define OP_EORI_B_DATA_EA     ASM("EORI.B\t#$%X,%s", SRC_WORD, DST_EA(1, 1, (ea_data & ea_alterable)))
#define OP_EORI_L_DATA_EA     ASM("EORI.L\t#$%08X,%s", SRC_LONG, DST_EA(4, 2, (ea_data & ea_alterable)))
#define OP_EORI_W_DATA_EA     ASM("EORI.W\t#$%04X,%s", SRC_WORD, DST_EA(2, 1, (ea_data & ea_alterable)))
#define OP_EORI_W_DATA_SR     ASM("EORI.W\t#$%04X,SR", SRC_WORD)
#define OP_EXG_AX_AY          ASM("EXG\t%s,%s", AX, AY)
#define OP_EXG_DX_AY          ASM("EXG\t%s,%s", DX, AY)
#define OP_EXG_DX_DY          ASM("EXG\t%s,%s", DX, DY)
#define OP_EXT_L_DN           ASM("EXT.L\t%s", DY)
#define OP_EXT_W_DN           ASM("EXT.W\t%s", DY)
#define OP_UNDEFINED                                                                                                   \
    do {                                                                                                               \
        goto illegal;                                                                                                  \
    } while (0)
#define OP_ILLEGAL             ASM("ILLEGAL")
#define OP_JMP_EA              ASM("JMP\t%s", DST_EA(4, 0, ea_control))
#define OP_JSR_EA              ASM("JSR\t%s", DST_EA(4, 0, ea_control))
#define OP_LEA_EA_AN           ASM("LEA\t%s,%s", SRC_EA(4, 0, ea_any), AN)
#define OP_LINK                ASM("LINK\t%s,#$%X", AY, DST_WORD)
#define OP_LINK_L_AN_DISP      ASM("LINK.L\t%s,#$%08X", AY, SRC_LONG)
#define OP_LSL_B_DATA_DY       ASM("LSL.B\t#$%X,%s", (int)IMM, DY);
#define OP_LSL_B_DX_DY         ASM("LSL.B\t%s,%s", DX, DY);
#define OP_LSL_L_DATA_DY       ASM("LSL.L\t#$%X,%s", (int)IMM, DY);
#define OP_LSL_L_DX_DY         ASM("LSL.L\t%s,%s", DX, DY);
#define OP_LSL_W_DATA_DY       ASM("LSL.W\t#$%X,%s", (int)IMM, DY);
#define OP_LSL_W_DX_DY         ASM("LSL.W\t%s,%s", DX, DY);
#define OP_LSL_W_EA            ASM("LSL.W\t%s", DST_EA(2, 0, (ea_memory & ea_alterable)));
#define OP_LSR_B_DATA_DY       ASM("LSR.B\t#$%X,%s", (int)IMM, DY);
#define OP_LSR_B_DX_DY         ASM("LSR.B\t%s,%s", DX, DY);
#define OP_LSR_L_DATA_DY       ASM("LSR.L\t#$%X,%s", (int)IMM, DY);
#define OP_LSR_L_DX_DY         ASM("LSR.L\t%s,%s", DX, DY);
#define OP_LSR_W_DATA_DY       ASM("LSR.W\t#$%X,%s", (int)IMM, DY);
#define OP_LSR_W_DX_DY         ASM("LSR.W\t%s,%s", DX, DY);
#define OP_LSR_W_EA            ASM("LSR.W\t%s", DST_EA(2, 0, (ea_memory & ea_alterable)));
#define OP_MOVE_AN_USP         ASM("MOVE\t%s,USP", AY)
#define OP_MOVE_B_CCR_EA       ASM("MOVE\tCCR,%s", DST_EA(2, 0, (ea_data & ea_alterable)));
#define OP_MOVE_B_EA_CCR       ASM("MOVE\t%s,CCR", SRC_EA(2, 0, ea_any));
#define OP_MOVE_B_EA_EA        ASM("MOVE.B\t%s,%s", SRC_EA(1, 0, ea_any), MOVE_DST(1))
#define OP_MOVE_EA_SR          ASM("MOVE\t%s,SR", SRC_EA(2, 0, ea_any))
#define OP_MOVE_L_EA_EA        ASM("MOVE.L\t%s,%s", SRC_EA(4, 0, ea_any), MOVE_DST(4))
#define OP_MOVE_USP_AN         ASM("MOVE\tUSP,%s", AY)
#define OP_MOVE_W_EA_EA        ASM("MOVE.W\t%s,%s", SRC_EA(2, 0, ea_any), MOVE_DST(2))
#define OP_MOVEA_L_EA_AN       ASM("MOVEA.L\t%s,%s", SRC_EA(4, 0, ea_any), AN);
#define OP_MOVEA_W_EA_AN       ASM("MOVEA.W\t%s,%s", SRC_EA(2, 0, ea_any), AN)
#define OP_MOVE_W_SR_EA        ASM("MOVE\tSR,%s", DST_EA(2, 0, (ea_data & ea_alterable)))
#define OP_MOVEC_RC_RN         ASM("MOVEC\t%s,%s", RC, RN)
#define OP_MOVEC_RN_RC         ASM("MOVEC\t%s,%s", RN, RC)
#define OP_MOVEM_L_EA_LIST     ASM("MOVEM.L\t%s,%s", SRC_EA(4, 1, ea_control | ea_an_plus), LIST)
#define OP_MOVEM_L_LIST_EA     ASM("MOVEM.L\t%s,%s", LIST, DST_EA(4, 1, (ea_control & ea_alterable) | ea_min_an))
#define OP_MOVEM_W_EA_LIST     ASM("MOVEM.W\t%s,%s", SRC_EA(2, 1, ea_control | ea_an_plus), LIST)
#define OP_MOVEM_W_LIST_EA     ASM("MOVEM.W\t%s,%s", LIST, DST_EA(2, 1, (ea_control & ea_alterable) | ea_min_an))
#define OP_MOVEP_L_D16AY_DX    ASM("MOVEP.L\t$%04X(%s),%s", SRC_WORD, AY, DX)
#define OP_MOVEP_L_DX_D16AY    ASM("MOVEP.L\t%s,$%04X(%s)", DX, SRC_WORD, AY)
#define OP_MOVEP_W_D16AY_DX    ASM("MOVEP.W\t$%04X(%s),%s", SRC_WORD, AY, DX)
#define OP_MOVEP_W_DX_D16AY    ASM("MOVEP.W\t%s,$%04X(%s)", DX, SRC_WORD, AY)
#define OP_MOVEQ_L_DATA_DN     ASM("MOVEQ\t#$%02X,%s", (int)(uint8_t)(opcode & 0xFF), DN)
#define OP_MOVES_B_EA_RN       ASM("MOVES.B\t%s,%s", SRC_EA(1, 1, (ea_memory & ea_alterable)), RN)
#define OP_MOVES_B_RN_EA       ASM("MOVES.B\t%s,%s", RN, DST_EA(1, 1, (ea_memory & ea_alterable)))
#define OP_MOVES_L_EA_RN       ASM("MOVES.L\t%s,%s", SRC_EA(4, 1, (ea_memory & ea_alterable)), RN)
#define OP_MOVES_L_RN_EA       ASM("MOVES.L\t%s,%s", RN, DST_EA(4, 1, (ea_memory & ea_alterable)))
#define OP_MOVES_W_EA_RN       ASM("MOVES.W\t%s,%s", SRC_EA(2, 1, (ea_memory & ea_alterable)), RN)
#define OP_MOVES_W_RN_EA       ASM("MOVES.W\t%s,%s", RN, DST_EA(2, 1, (ea_memory & ea_alterable)))
#define OP_MULS_L_EA_DH_DL     ASM("MULS.L\t%s,%s:%s", SRC_EA(4, 1, ea_any), DH, DL)
#define OP_MULS_W_EA_DN        ASM("MULS.W\t%s,%s", SRC_EA(2, 0, ea_any), DN)
#define OP_MULU_W_EA_DN        ASM("MULU.W\t%s,%s", SRC_EA(2, 0, ea_data), DN)
#define OP_NBCD_B_EA           ASM("NBCD\t%s", DST_EA(1, 0, (ea_data & ea_alterable)))
#define OP_NEG_B_EA            ASM("NEG.B\t%s", DST_EA(1, 0, (ea_data & ea_alterable)))
#define OP_NEG_L_EA            ASM("NEG.L\t%s", DST_EA(4, 0, (ea_data & ea_alterable)))
#define OP_NEG_W_EA            ASM("NEG.W\t%s", DST_EA(2, 0, (ea_data & ea_alterable)))
#define OP_NEGX_B_EA           ASM("NEGX.B\t%s", DST_EA(1, 0, (ea_data & ea_alterable)))
#define OP_NEGX_L_EA           ASM("NEGX.L\t%s", DST_EA(4, 0, (ea_data & ea_alterable)))
#define OP_NEGX_W_EA           ASM("NEGX.W\t%s", DST_EA(2, 0, (ea_data & ea_alterable)))
#define OP_NOP                 ASM("NOP")
#define OP_NOT_B_EA            ASM("NOT.B\t%s", DST_EA(1, 0, (ea_data & ea_alterable)))
#define OP_NOT_L_EA            ASM("NOT.L\t%s", DST_EA(4, 0, (ea_data & ea_alterable)))
#define OP_NOT_W_EA            ASM("NOT.W\t%s", DST_EA(2, 0, (ea_data & ea_alterable)))
#define OP_OR_B_DN_EA          ASM("OR.B\t%s,%s", DN, DST_EA(1, 0, (ea_memory & ea_alterable)))
#define OP_OR_B_EA_DN          ASM("OR.B\t%s,%s", SRC_EA(2, 0, ea_data), DN)
#define OP_OR_L_DN_EA          ASM("OR.L\t%s,%s", DN, DST_EA(4, 0, (ea_memory & ea_alterable)))
#define OP_OR_L_EA_DN          ASM("OR.L\t%s,%s", SRC_EA(4, 0, ea_data), DN)
#define OP_OR_W_DN_EA          ASM("OR.W\t%s,%s", DN, DST_EA(2, 0, (ea_memory & ea_alterable)))
#define OP_OR_W_EA_DN          ASM("OR.W\t%s,%s", SRC_EA(2, 0, ea_data), DN)
#define OP_ORI_B_DATA_CCR      ASM("ORI.B\t#$%04X,CCR", SRC_WORD)
#define OP_ORI_B_DATA_EA       ASM("ORI.B\t#$%X,%s", SRC_WORD, DST_EA(1, 1, (ea_data & ea_alterable)))
#define OP_ORI_L_DATA_EA       ASM("ORI.L\t#$%08X,%s", SRC_LONG, DST_EA(4, 2, (ea_data & ea_alterable)))
#define OP_ORI_W_DATA_EA       ASM("ORI.W\t#$%04X,%s", SRC_WORD, DST_EA(2, 1, (ea_data & ea_alterable)))
#define OP_ORI_W_DATA_SR       ASM("ORI.W\t#$%04X,SR", SRC_WORD)
#define OP_PEA_EA              ASM("PEA\t%s", DST_EA(4, 0, ea_control))
#define OP_RESET               ASM("RESET")
#define OP_ROL_B_DATA_DY       ASM("ROL.B\t#$%X,%s", (int)IMM, DY);
#define OP_ROL_B_DX_DY         ASM("ROL.B\t%s,%s", DX, DY);
#define OP_ROL_L_DATA_DY       ASM("ROL.L\t#$%X,%s", (int)IMM, DY);
#define OP_ROL_L_DX_DY         ASM("ROL.L\t%s,%s", DX, DY);
#define OP_ROL_W_DATA_DY       ASM("ROL.W\t#$%X,%s", (int)IMM, DY);
#define OP_ROL_W_DX_DY         ASM("ROL.W\t%s,%s", DX, DY);
#define OP_ROL_W_EA            ASM("ROL.W\t%s", DST_EA(2, 0, (ea_memory & ea_alterable)));
#define OP_ROR_B_DATA_DY       ASM("ROR.B\t#$%X,%s", (int)IMM, DY);
#define OP_ROR_B_DX_DY         ASM("ROR.B\t%s,%s", DX, DY);
#define OP_ROR_L_DATA_DY       ASM("ROR.L\t#$%X,%s", (int)IMM, DY);
#define OP_ROR_L_DX_DY         ASM("ROR.L\t%s,%s", DX, DY);
#define OP_ROR_W_DATA_DY       ASM("ROR.W\t#$%X,%s", (int)IMM, DY);
#define OP_ROR_W_DX_DY         ASM("ROR.W\t%s,%s", DX, DY);
#define OP_ROR_W_EA            ASM("ROR.W\t%s", DST_EA(2, 0, (ea_memory & ea_alterable)));
#define OP_ROXL_B_DATA_DY      ASM("ROXL.B\t#$%X,%s", (int)(IMM), DY);
#define OP_ROXL_B_DX_DY        ASM("ROXL.B\t%s,%s", DX, DY);
#define OP_ROXL_L_DATA_DY      ASM("ROXL.L\t#$%X,%s", (int)(IMM), DY);
#define OP_ROXL_L_DX_DY        ASM("ROXL.L\t%s,%s", DX, DY);
#define OP_ROXL_W_DATA_DY      ASM("ROXL.W\t#$%X,%s", (int)(IMM), DY);
#define OP_ROXL_W_DX_DY        ASM("ROXL.W\t%s,%s", DX, DY);
#define OP_ROXL_W_EA           ASM("ROXL.W\t%s", DST_EA(2, 0, (ea_memory & ea_alterable)));
#define OP_ROXR_B_DATA_DY      ASM("ROXR.B\t#$%X,%s", (int)(IMM), DY);
#define OP_ROXR_B_DX_DY        ASM("ROXR.B\t%s,%s", DX, DY);
#define OP_ROXR_L_DATA_DY      ASM("ROXR.L\t#$%X,%s", (int)(IMM), DY);
#define OP_ROXR_L_DX_DY        ASM("ROXR.L\t%s,%s", DX, DY);
#define OP_ROXR_W_DATA_DY      ASM("ROXR.W\t#$%X,%s", (int)(IMM), DY);
#define OP_ROXR_W_DX_DY        ASM("ROXR.W\t%s,%s", DX, DY);
#define OP_ROXR_W_EA           ASM("ROXR.W\t%s", DST_EA(2, 0, (ea_memory & ea_alterable)));
#define OP_RTD_DISPLACEMENT    ASM("RTD\t#$%X", DST_WORD)
#define OP_RTE                 ASM("RTE")
#define OP_RTM_RN              ASM("RTM\t%s", opcode & 8 ? AY : DY)
#define OP_RTR                 ASM("RTR")
#define OP_RTS                 ASM("RTS")
#define OP_SBCD_AX_AY          ASM("SBCD\t-(%s),-(%s)", AY, AX)
#define OP_SBCD_DX_DY          ASM("SBCD\t%s,%s", DY, DX)
#define OP_PACK_DY_DX          ASM("PACK\t%s,%s,#$%04X", DY, DX, SRC_WORD)
#define OP_PACK_AY_AX          ASM("PACK\t-(%s),-(%s),#$%04X", AY, AX, SRC_WORD)
#define OP_UNPK_DY_DX          ASM("UNPK\t%s,%s,#$%04X", DY, DX, SRC_WORD)
#define OP_UNPK_AY_AX          ASM("UNPK\t-(%s),-(%s),#$%04X", AY, AX, SRC_WORD)
#define OP_SCC_EA              ASM("S%s\t%s", CC, DST_EA(1, 0, (ea_data & ea_alterable)));
#define OP_STOP_DATA           ASM("STOP\t#$%X", SRC_WORD)
#define OP_SUB_B_DN_EA         ASM("SUB.B\t%s,%s", DN, DST_EA(1, 0, (ea_memory & ea_alterable)));
#define OP_SUB_B_EA_DN         ASM("SUB.B\t%s,%s", SRC_EA(1, 0, ea_any & ~ea_an), DN);
#define OP_SUB_L_DN_EA         ASM("SUB.L\t%s,%s", DN, DST_EA(4, 0, (ea_memory & ea_alterable)));
#define OP_SUB_L_EA_DN         ASM("SUB.L\t%s,%s", SRC_EA(4, 0, ea_any), DN);
#define OP_SUB_W_DN_EA         ASM("SUB.W\t%s,%s", DN, DST_EA(2, 0, (ea_memory & ea_alterable)));
#define OP_SUB_W_EA_DN         ASM("SUB.W\t%s,%s", SRC_EA(2, 0, ea_any), DN);
#define OP_SUBA_L_EA_AN        ASM("SUBA.L\t%s,%s", SRC_EA(4, 0, ea_any), AN);
#define OP_SUBA_W_EA_AN        ASM("SUBA.W\t%s,%s", SRC_EA(2, 0, ea_any), AN);
#define OP_SUBI_B_DATA_EA      ASM("SUBI.B\t#$%X,%s", SRC_WORD, DST_EA(1, 1, (ea_data & ea_alterable)));
#define OP_SUBI_L_DATA_EA      ASM("SUBI.L\t#$%08X,%s", SRC_LONG, DST_EA(4, 2, (ea_data & ea_alterable)));
#define OP_SUBI_W_DATA_EA      ASM("SUBI.W\t#$%04X,%s", SRC_WORD, DST_EA(2, 1, (ea_data & ea_alterable)));
#define OP_SUBQ_B_DATA_EA      ASM("SUBQ.B\t#$%X,%s", (int)IMM, DST_EA(1, 0, ea_alterable & ~ea_an))
#define OP_SUBQ_L_DATA_AN      ASM("SUBQ.L\t#$%X,%s", (int)IMM, AY)
#define OP_SUBQ_L_DATA_EA      ASM("SUBQ.L\t#$%X,%s", (int)IMM, DST_EA(4, 0, ea_alterable & ~ea_an))
#define OP_SUBQ_W_DATA_AN      ASM("SUBQ.W\t#$%X,%s", (int)IMM, AY)
#define OP_SUBQ_W_DATA_EA      ASM("SUBQ.W\t#$%X,%s", (int)IMM, DST_EA(2, 0, ea_alterable & ~ea_an))
#define OP_SUBX_B_AX_AY        ASM("SUBX.B\t-(%s),-(%s)", AY, AX);
#define OP_SUBX_B_DX_DY        ASM("SUBX.B\t%s,%s", DY, DX);
#define OP_SUBX_L_AX_AY        ASM("SUBX.L\t-(%s),-(%s)", AY, AX);
#define OP_SUBX_L_DX_DY        ASM("SUBX.L\t%s,%s", DY, DX);
#define OP_SUBX_W_AX_AY        ASM("SUBX.W\t-(%s),-(%s)", AY, AX);
#define OP_SUBX_W_DX_DY        ASM("SUBX.W\t%s,%s", DY, DX);
#define OP_SWAP_DN             ASM("SWAP\t%s", DY)
#define OP_TAS_B_EA            ASM("TAS\t%s", DST_EA(1, 0, ea_any))
#define OP_TRAP_VECTOR         ASM("TRAP\t#$%X", (int)(opcode & 0xF))
#define OP_TRAPV               ASM("TRAPV")
#define OP_TRAPCC              ASM("T%s", CC)
#define OP_TRAPCC_W_DATA       ASM("TP%s.W\t#$%X", CC, (int)SRC_WORD)
#define OP_TRAPCC_L_DATA       ASM("TP%s.L\t#$%08X", CC, (int)SRC_LONG)
#define OP_TST_B_EA            ASM("TST.B\t%s", DST_EA(1, 0, ea_any))
#define OP_TST_L_EA            ASM("TST.L\t%s", DST_EA(4, 0, ea_any))
#define OP_TST_W_EA            ASM("TST.W\t%s", DST_EA(2, 0, ea_any))
#define OP_UNLK                ASM("UNLK\t%s", AY)
#define OP_PBCC_W              ASM("PB%s\t%s", PBCC, PC_DISP)
#define OP_PBCC_L              ASM("PB%s.L\t%s", PBCC, LONG_PC_DISP)
#define OP_PSAVE_EA            ASM("PSAVE\t%s", DST_EA(4, 0, (ea_control | ea_min_an) & ea_alterable))
#define OP_PRESTORE_EA         ASM("PRESTORE\t%s", DST_EA(4, 0, (ea_control | ea_an_plus)))
#define OP_FBCC_W_DISPLACEMENT ASM("FB%s\t%s", FBCC, PC_DISP)
#define OP_FBCC_L_DISPLACEMENT ASM("FB%s.L\t%s", FBCC, LONG_PC_DISP)
#define OP_FSAVE_EA            ASM("FSAVE\t%s", DST_EA(4, 0, (ea_control | ea_min_an) & ea_alterable))
#define OP_FRESTORE_EA         ASM("FRESTORE\t%s", DST_EA(4, 0, (ea_control | ea_an_plus)))
#define OP_CINVL_CACHES_AN     ASM("CINVL\t%s,(%s)", CACHES, AY)
#define OP_CINVP_CACHES_AN     ASM("CINVP\t%s,(%s)", CACHES, AY)
#define OP_CINVA_CACHES        ASM("CINVA\t%s", CACHES)
#define OP_CPUSHL_CACHES_AN    ASM("CPUSHL\t%s,(%s)", CACHES, AY)
#define OP_CPUSHP_CACHES_AN    ASM("CPUSHP\t%s,(%s)", CACHES, AY)
#define OP_CPUSHA_CACHES       ASM("CPUSHA\t%s", CACHES)
#define OP_PFLUSH_AN           ASM("PFLUSH\t(%s)", AY)
#define OP_PFLUSHN_AN          ASM("PFLUSHN\t(%s)", AY)
#define OP_PFLUSHA             ASM("PFLUSHA")
#define OP_PFLUSHAN            ASM("PFLUSHAN")
#define OP_PTESTR_AN           ASM("PTESTR\t(%s)", AY)
#define OP_PTESTW_AN           ASM("PTESTW\t(%s)", AY)
#define OP_MOVE16_AX_AY        ASM("MOVE16\t(%s)+,(%s)+", AY, AX)
#define OP_MOVE16_AN_P_XXX_L   ASM("MOVE16\t(%s)+,$%08X", AY, DST_LONG)
#define OP_MOVE16_XXX_L_AN_P   ASM("MOVE16\t$%08X,(%s)+", SRC_LONG, AY)
#define OP_MOVE16_AN_XXX_L     ASM("MOVE16\t(%s),$%08X", AY, DST_LONG)
#define OP_MOVE16_XXX_L_AN     ASM("MOVE16\t$%08X,(%s)", SRC_LONG, AY)
#define OP_PMMU_GENERAL        ASM("PMMU") // generic disasm for PMOVE/PFLUSH/PTEST
#define OP_FTRAP               OP_UNDEFINED

#define CPU_DECODER_NAME        cpu_disasm
#define CPU_DECODER_RETURN_TYPE int
#define CPU_DECODER_ARGS        uint16_t *instr, char *buf
#define CPU_DECODER_PROLOGUE                                                                                           \
    uint16_t opcode = instr[0];                                                                                        \
    uint16_t ext_word = instr[1];                                                                                      \
    uint16_t *fetch_pos_dst = instr;                                                                                   \
    uint16_t *fetch_pos_src = instr;                                                                                   \
    buf[0] = '\0';
#define CPU_DECODER_EPILOGUE                                                                                           \
    if (0) {                                                                                                           \
    illegal:;                                                                                                          \
    }                                                                                                                  \
    done:                                                                                                              \
    if (buf && (buf[0] == '\0' || strstr(buf, "<illegal>") != NULL)) {                                                 \
        sprintf(buf, "DC.W\t$%04X", (unsigned int)instr[0]);                                                           \
        return 1;                                                                                                      \
    }                                                                                                                  \
    if (fetch_pos_dst > fetch_pos_src)                                                                                 \
        return (int)MAX(fetch_pos_dst - instr, 1);                                                                     \
    else                                                                                                               \
        return (int)MAX(fetch_pos_src - instr, 1);

#include "cpu_decode.h"
