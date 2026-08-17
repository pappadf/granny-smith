// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_disasm.c
// Dependency-free MPC601 disassembler.  Encodings per the 601UM chapter-10
// instruction pages (including the POWER holdovers and the 601 SPR map);
// output uses standard mnemonics with the common simplified forms (li, lis,
// mr, nop, blr, bctr, cmpwi, mflr, ...) the way the development oracle
// (powerpc-linux-gnu-objdump -m powerpc:601) prints them.

#include "ppc_disasm.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// === Field extraction (BE numbering) ===
#define F_OPCD(w) ((w) >> 26)
#define F_RT(w)   (((w) >> 21) & 31)
#define F_RA(w)   (((w) >> 16) & 31)
#define F_RB(w)   (((w) >> 11) & 31)
#define F_XO10(w) (((w) >> 1) & 0x3FF)
#define F_XO9(w)  (((w) >> 1) & 0x1FF)
#define F_OE(w)   (((w) >> 10) & 1)
#define F_RC(w)   ((w) & 1)
#define F_SIMM(w) ((int32_t)(int16_t)(w))
#define F_UIMM(w) ((w) & 0xFFFFu)
#define F_MB(w)   (((w) >> 6) & 31)
#define F_ME(w)   (((w) >> 1) & 31)
#define F_CRFD(w) (((w) >> 23) & 7)

// Emit "mnemonic\toperands" into out->text.
static void emit(ppc_insn *o, const char *mnem, const char *fmt, ...) {
    size_t n = 0;
    while (mnem[n] && n < sizeof(o->text) - 2) {
        o->text[n] = mnem[n];
        n++;
    }
    o->text[n] = '\0';
    if (fmt && fmt[0]) {
        char ops[80];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(ops, sizeof(ops), fmt, ap);
        va_end(ap);
        if (ops[0] && n < sizeof(o->text) - 2) {
            o->text[n++] = '\t';
            snprintf(o->text + n, sizeof(o->text) - n, "%s", ops);
        }
    }
}

static void invalid(ppc_insn *o) {
    o->status = PPC_DIS_INVALID;
    snprintf(o->text, sizeof(o->text), ".long\t$%08X", (unsigned)o->word);
}

// mnemonic + optional "o" (OE) and "." (Rc) suffixes
static const char *suffix_oe_rc(char *buf, size_t bufsz, const char *base, uint32_t w, int has_oe) {
    snprintf(buf, bufsz, "%s%s%s", base, (has_oe && F_OE(w)) ? "o" : "", F_RC(w) ? "." : "");
    return buf;
}

static const char *suffix_rc(char *buf, size_t bufsz, const char *base, uint32_t w) {
    snprintf(buf, bufsz, "%s%s", base, F_RC(w) ? "." : "");
    return buf;
}

// SPR name per the 601 encoding tables (10-4/10-5); NULL if unknown.
static const char *spr_name(uint32_t n) {
    switch (n) {
    case 0:
        return "mq";
    case 1:
        return "xer";
    case 4:
        return "rtcu";
    case 5:
        return "rtcl";
    case 6:
        return "dec"; // POWER user-read encoding
    case 8:
        return "lr";
    case 9:
        return "ctr";
    case 18:
        return "dsisr";
    case 19:
        return "dar";
    case 20:
        return "rtcu"; // write encoding
    case 21:
        return "rtcl";
    case 22:
        return "dec";
    case 25:
        return "sdr1";
    case 26:
        return "srr0";
    case 27:
        return "srr1";
    case 272:
        return "sprg0";
    case 273:
        return "sprg1";
    case 274:
        return "sprg2";
    case 275:
        return "sprg3";
    case 282:
        return "ear";
    case 287:
        return "pvr";
    case 528:
        return "ibat0u";
    case 529:
        return "ibat0l";
    case 530:
        return "ibat1u";
    case 531:
        return "ibat1l";
    case 532:
        return "ibat2u";
    case 533:
        return "ibat2l";
    case 534:
        return "ibat3u";
    case 535:
        return "ibat3l";
    case 1008:
        return "hid0";
    case 1009:
        return "hid1";
    case 1010:
        return "iabr";
    case 1013:
        return "dabr";
    case 1023:
        return "pir";
    default:
        return NULL;
    }
}

// Valid BO encodings: the z bits of the 1z00y/1z01y/1z1zz groups must be
// zero (invalid forms otherwise); the low bits of the condition-only groups
// are prediction hints and always decode.
static int bo_valid(uint32_t bo) {
    if (bo & 0x10u) // 1z00y / 1z01y / 1z1zz groups
        return (bo & 0x04u) ? bo == 0x14u : (bo & 0x08u) == 0;
    return 1; // 0000y/0001y/001at/0100y/0101y/011at
}

// Conditional-branch decode shared by bc / bclr / bcctr.  `kind` is 0 for
// bc (pc-relative/absolute target), 1 for lr, 2 for ctr.
static void dis_bcond(ppc_insn *o, uint32_t w, uint32_t addr, int kind) {
    static const char *const t_names[4] = {"lt", "gt", "eq", "so"};
    static const char *const f_names[4] = {"ge", "le", "ne", "ns"};
    uint32_t bo = F_RT(w), bi = F_RA(w);
    if (!bo_valid(bo)) {
        invalid(o);
        return;
    }
    const char *lk = F_RC(w) ? "l" : "";
    const char *tail = (kind == 1) ? "lr" : (kind == 2) ? "ctr" : "";
    char mnem[24], ops[64];
    ops[0] = '\0';
    o->is_branch = 1;

    // Resolved target for the bc form
    char tgt[24];
    tgt[0] = '\0';
    if (kind == 0) {
        int32_t bd = (int32_t)(int16_t)(w & 0xFFFCu);
        o->target = (w & 2u) ? (uint32_t)bd : addr + (uint32_t)bd;
        o->has_target = 1;
        snprintf(tgt, sizeof(tgt), "$%08X", (unsigned)o->target);
    }

    // The BI field only reads meaningfully when the condition is tested, so
    // the condition-ignoring forms simplify only with BI=0.
    if ((bo & 0x14u) == 0x14u && bi == 0) { // branch always
        if (kind == 0)
            snprintf(mnem, sizeof(mnem), "b%s%s", lk, (w & 2u) ? "a" : "");
        else
            snprintf(mnem, sizeof(mnem), "b%s%s", tail, lk);
        emit(o, mnem, "%s", tgt);
        return;
    }
    if ((bo & 0x1Cu) == 0x0Cu || (bo & 0x1Cu) == 0x04u) { // condition test, no CTR
        const char *cond = ((bo & 8u) ? t_names : f_names)[bi & 3u];
        snprintf(mnem, sizeof(mnem), "b%s%s%s%s", cond, tail, lk, (kind == 0 && (w & 2u)) ? "a" : "");
        if (bi >= 4)
            snprintf(ops, sizeof(ops), "cr%u%s%s", bi >> 2, tgt[0] ? "," : "", tgt);
        else
            snprintf(ops, sizeof(ops), "%s", tgt);
        emit(o, mnem, "%s", ops);
        return;
    }
    if ((bo & 0x16u) == 0x10u || (bo & 0x16u) == 0x12u) { // CTR-only forms
        if (bi == 0) {
            snprintf(mnem, sizeof(mnem), "%s%s%s%s", (bo & 2u) ? "bdz" : "bdnz", tail, lk,
                     (kind == 0 && (w & 2u)) ? "a" : "");
            emit(o, mnem, "%s", tgt);
            return;
        }
    }
    // Everything else (incl. CTR+condition combinations): generic bc form
    snprintf(mnem, sizeof(mnem), "bc%s%s%s", tail, lk, (kind == 0 && (w & 2u)) ? "a" : "");
    snprintf(ops, sizeof(ops), "%u,%u%s%s", bo, bi, tgt[0] ? "," : "", tgt);
    emit(o, mnem, "%s", ops);
}

// D-form load/store: "op rT,disp(rA)" (FP registers use fN)
static void dis_dform_mem(ppc_insn *o, uint32_t w, const char *mnem, int fp) {
    if (F_RA(w))
        emit(o, mnem, "%c%u,%d(r%u)", fp ? 'f' : 'r', (unsigned)F_RT(w), (int)F_SIMM(w), (unsigned)F_RA(w));
    else
        emit(o, mnem, "%c%u,%d(0)", fp ? 'f' : 'r', (unsigned)F_RT(w), (int)F_SIMM(w));
}

// X-form load/store: "opx rT,rA,rB" with rA=0 printed as 0
static void dis_xform_mem(ppc_insn *o, uint32_t w, const char *mnem, int fp) {
    char ra[8];
    if (F_RA(w))
        snprintf(ra, sizeof(ra), "r%u", (unsigned)F_RA(w));
    else
        snprintf(ra, sizeof(ra), "0");
    emit(o, mnem, "%c%u,%s,r%u", fp ? 'f' : 'r', (unsigned)F_RT(w), ra, (unsigned)F_RB(w));
}

// === opcode 19 ===
static void dis_op19(ppc_insn *o, uint32_t w, uint32_t addr) {
    static const struct {
        uint16_t xo;
        const char *mnem;
    } crops[] = {
        {33,  "crnor" },
        {129, "crandc"},
        {193, "crxor" },
        {225, "crnand"},
        {257, "crand" },
        {289, "creqv" },
        {417, "crorc" },
        {449, "cror"  },
    };
    uint32_t xo = F_XO10(w);
    switch (xo) {
    case 0:
        emit(o, "mcrf", "cr%u,cr%u", (unsigned)F_CRFD(w), (unsigned)((w >> 18) & 7));
        return;
    case 16:
        dis_bcond(o, w, addr, 1);
        return;
    case 528:
        dis_bcond(o, w, addr, 2);
        return;
    case 50:
        o->is_branch = 1;
        emit(o, "rfi", "");
        return;
    case 150:
        emit(o, "isync", "");
        return;
    default:
        for (unsigned i = 0; i < sizeof(crops) / sizeof(crops[0]); i++) {
            if (crops[i].xo == xo) {
                // crclr/crset/crmove/crnot simplified forms exist; keep the
                // canonical form (corpus normalizes)
                emit(o, crops[i].mnem, "%u,%u,%u", (unsigned)F_RT(w), (unsigned)F_RA(w), (unsigned)F_RB(w));
                return;
            }
        }
        invalid(o);
        return;
    }
}

// === opcode 31 ===

// Table-driven arithmetic/logical/shift rows
typedef enum {
    OPS_RT_RA_RB, // rD,rA,rB (XO-form arithmetic)
    OPS_RT_RA, // rD,rA
    OPS_RA_RS_RB, // rA,rS,rB (logical/shift: source is the RT field)
    OPS_RA_RS_SH, // rA,rS,SH
    OPS_RA_RS, // rA,rS (extsb/extsh/cntlzw)
} op31_operands_t;

static const struct {
    uint16_t xo; // 9-bit xo for has_oe rows, 10-bit otherwise
    uint8_t has_oe;
    uint8_t power; // POWER holdover
    uint8_t operands;
    const char *mnem;
} op31_alu[] = {
    {266, 1, 0, OPS_RT_RA_RB, "add"   },
    {10,  1, 0, OPS_RT_RA_RB, "addc"  },
    {138, 1, 0, OPS_RT_RA_RB, "adde"  },
    {234, 1, 0, OPS_RT_RA,    "addme" },
    {202, 1, 0, OPS_RT_RA,    "addze" },
    {40,  1, 0, OPS_RT_RA_RB, "subf"  },
    {8,   1, 0, OPS_RT_RA_RB, "subfc" },
    {136, 1, 0, OPS_RT_RA_RB, "subfe" },
    {232, 1, 0, OPS_RT_RA,    "subfme"},
    {200, 1, 0, OPS_RT_RA,    "subfze"},
    {104, 1, 0, OPS_RT_RA,    "neg"   },
    {75,  0, 0, OPS_RT_RA_RB, "mulhw" },
    {11,  0, 0, OPS_RT_RA_RB, "mulhwu"},
    {235, 1, 0, OPS_RT_RA_RB, "mullw" },
    {491, 1, 0, OPS_RT_RA_RB, "divw"  },
    {459, 1, 0, OPS_RT_RA_RB, "divwu" },
    {360, 1, 1, OPS_RT_RA,    "abs"   },
    {488, 1, 1, OPS_RT_RA,    "nabs"  },
    {264, 1, 1, OPS_RT_RA_RB, "doz"   },
    {107, 1, 1, OPS_RT_RA_RB, "mul"   },
    {331, 1, 1, OPS_RT_RA_RB, "div"   },
    {363, 1, 1, OPS_RT_RA_RB, "divs"  },
    {28,  0, 0, OPS_RA_RS_RB, "and"   },
    {60,  0, 0, OPS_RA_RS_RB, "andc"  },
    {444, 0, 0, OPS_RA_RS_RB, "or"    },
    {412, 0, 0, OPS_RA_RS_RB, "orc"   },
    {316, 0, 0, OPS_RA_RS_RB, "xor"   },
    {476, 0, 0, OPS_RA_RS_RB, "nand"  },
    {124, 0, 0, OPS_RA_RS_RB, "nor"   },
    {284, 0, 0, OPS_RA_RS_RB, "eqv"   },
    {954, 0, 0, OPS_RA_RS,    "extsb" },
    {922, 0, 0, OPS_RA_RS,    "extsh" },
    {26,  0, 0, OPS_RA_RS,    "cntlzw"},
    {24,  0, 0, OPS_RA_RS_RB, "slw"   },
    {536, 0, 0, OPS_RA_RS_RB, "srw"   },
    {792, 0, 0, OPS_RA_RS_RB, "sraw"  },
    {824, 0, 0, OPS_RA_RS_SH, "srawi" },
    {29,  0, 1, OPS_RA_RS_RB, "maskg" },
    {541, 0, 1, OPS_RA_RS_RB, "maskir"},
    {537, 0, 1, OPS_RA_RS_RB, "rrib"  },
    {153, 0, 1, OPS_RA_RS_RB, "sle"   },
    {217, 0, 1, OPS_RA_RS_RB, "sleq"  },
    {184, 0, 1, OPS_RA_RS_SH, "sliq"  },
    {248, 0, 1, OPS_RA_RS_SH, "slliq" },
    {216, 0, 1, OPS_RA_RS_RB, "sllq"  },
    {152, 0, 1, OPS_RA_RS_RB, "slq"   },
    {664, 0, 1, OPS_RA_RS_RB, "srq"   },
    {665, 0, 1, OPS_RA_RS_RB, "sre"   },
    {696, 0, 1, OPS_RA_RS_SH, "sriq"  },
    {728, 0, 1, OPS_RA_RS_RB, "srlq"  },
    {729, 0, 1, OPS_RA_RS_RB, "sreq"  },
    {760, 0, 1, OPS_RA_RS_SH, "srliq" },
    {920, 0, 1, OPS_RA_RS_RB, "sraq"  },
    {952, 0, 1, OPS_RA_RS_SH, "sraiq" },
    {921, 0, 1, OPS_RA_RS_RB, "srea"  },
    {531, 0, 1, OPS_RT_RA,    "clcs"  },
};

// X-form memory rows (10-bit xo, Rc must be 0 except stwcx.)
static const struct {
    uint16_t xo;
    uint8_t fp;
    const char *mnem;
} op31_mem[] = {
    {23,  0, "lwzx"  },
    {55,  0, "lwzux" },
    {87,  0, "lbzx"  },
    {119, 0, "lbzux" },
    {279, 0, "lhzx"  },
    {311, 0, "lhzux" },
    {343, 0, "lhax"  },
    {375, 0, "lhaux" },
    {151, 0, "stwx"  },
    {183, 0, "stwux" },
    {215, 0, "stbx"  },
    {247, 0, "stbux" },
    {407, 0, "sthx"  },
    {439, 0, "sthux" },
    {534, 0, "lwbrx" },
    {790, 0, "lhbrx" },
    {662, 0, "stwbrx"},
    {918, 0, "sthbrx"},
    {20,  0, "lwarx" },
    {310, 0, "eciwx" },
    {438, 0, "ecowx" },
    {535, 1, "lfsx"  },
    {567, 1, "lfsux" },
    {599, 1, "lfdx"  },
    {631, 1, "lfdux" },
    {663, 1, "stfsx" },
    {695, 1, "stfsux"},
    {727, 1, "stfdx" },
    {759, 1, "stfdux"},
    {277, 0, "lscbx" },
    {533, 0, "lswx"  },
    {661, 0, "stswx" },
};

static void dis_op31(ppc_insn *o, uint32_t w, uint32_t addr) {
    (void)addr;
    uint32_t xo = F_XO10(w);
    char m[16];

    // ALU table: compare against the 9-bit xo for OE-capable rows.
    for (unsigned i = 0; i < sizeof(op31_alu) / sizeof(op31_alu[0]); i++) {
        uint32_t row_xo = op31_alu[i].xo;
        int hit = op31_alu[i].has_oe ? (F_XO9(w) == row_xo) : (xo == row_xo);
        if (!hit)
            continue;
        o->is_power = op31_alu[i].power;
        suffix_oe_rc(m, sizeof(m), op31_alu[i].mnem, w, op31_alu[i].has_oe);
        switch (op31_alu[i].operands) {
        case OPS_RT_RA_RB:
            emit(o, m, "r%u,r%u,r%u", (unsigned)F_RT(w), (unsigned)F_RA(w), (unsigned)F_RB(w));
            return;
        case OPS_RT_RA:
            if (F_RB(w)) { // reserved field
                invalid(o);
                return;
            }
            emit(o, m, "r%u,r%u", (unsigned)F_RT(w), (unsigned)F_RA(w));
            return;
        case OPS_RA_RS_RB:
            // `or ra,rs,rs` is the canonical register move
            if (xo == 444 && F_RT(w) == F_RB(w) && !F_RC(w)) {
                emit(o, "mr", "r%u,r%u", (unsigned)F_RA(w), (unsigned)F_RT(w));
                return;
            }
            emit(o, m, "r%u,r%u,r%u", (unsigned)F_RA(w), (unsigned)F_RT(w), (unsigned)F_RB(w));
            return;
        case OPS_RA_RS_SH:
            emit(o, m, "r%u,r%u,%u", (unsigned)F_RA(w), (unsigned)F_RT(w), (unsigned)F_RB(w));
            return;
        case OPS_RA_RS:
            emit(o, m, "r%u,r%u", (unsigned)F_RA(w), (unsigned)F_RT(w));
            return;
        }
    }

    // Memory table
    for (unsigned i = 0; i < sizeof(op31_mem) / sizeof(op31_mem[0]); i++) {
        if (xo == op31_mem[i].xo && !F_RC(w)) {
            o->is_power = (xo == 277); // lscbx
            dis_xform_mem(o, w, op31_mem[i].mnem, op31_mem[i].fp);
            return;
        }
    }
    if (xo == 277 && F_RC(w)) { // lscbx.
        o->is_power = 1;
        dis_xform_mem(o, w, "lscbx.", 0);
        return;
    }

    switch (xo) {
    case 0:
        emit(o, "cmpw", "cr%u,r%u,r%u", (unsigned)F_CRFD(w), (unsigned)F_RA(w), (unsigned)F_RB(w));
        return;
    case 32:
        emit(o, "cmplw", "cr%u,r%u,r%u", (unsigned)F_CRFD(w), (unsigned)F_RA(w), (unsigned)F_RB(w));
        return;
    case 4:
        emit(o, "tw", "%u,r%u,r%u", (unsigned)F_RT(w), (unsigned)F_RA(w), (unsigned)F_RB(w));
        return;
    case 19:
        emit(o, "mfcr", "r%u", (unsigned)F_RT(w));
        return;
    case 144:
        emit(o, "mtcrf", "%u,r%u", (unsigned)((w >> 12) & 0xFF), (unsigned)F_RT(w));
        return;
    case 512:
        emit(o, "mcrxr", "cr%u", (unsigned)F_CRFD(w));
        return;
    case 339:
    case 467: {
        uint32_t n = ((w >> 16) & 0x1Fu) | (((w >> 11) & 0x1Fu) << 5);
        const char *name = spr_name(n);
        int is_mf = (xo == 339);
        // The canonical short forms for the user SPRs
        if (name && (n == 1 || n == 8 || n == 9)) {
            char mn[12];
            snprintf(mn, sizeof(mn), "%s%s", is_mf ? "mf" : "mt", name);
            emit(o, mn, "r%u", (unsigned)F_RT(w));
            return;
        }
        if (n == 0 || n == 4 || n == 5 || n == 20 || n == 21)
            o->is_power = 1; // MQ/RTC encodings are 601-specific
        char ops[32];
        if (is_mf) {
            if (name)
                snprintf(ops, sizeof(ops), "r%u,%s", (unsigned)F_RT(w), name);
            else
                snprintf(ops, sizeof(ops), "r%u,%u", (unsigned)F_RT(w), (unsigned)n);
            emit(o, "mfspr", "%s", ops);
        } else {
            if (name)
                snprintf(ops, sizeof(ops), "%s,r%u", name, (unsigned)F_RT(w));
            else
                snprintf(ops, sizeof(ops), "%u,r%u", (unsigned)n, (unsigned)F_RT(w));
            emit(o, "mtspr", "%s", ops);
        }
        return;
    }
    case 83:
        emit(o, "mfmsr", "r%u", (unsigned)F_RT(w));
        return;
    case 146:
        emit(o, "mtmsr", "r%u", (unsigned)F_RT(w));
        return;
    case 595:
        emit(o, "mfsr", "r%u,%u", (unsigned)F_RT(w), (unsigned)((w >> 16) & 0xF));
        return;
    case 210:
        emit(o, "mtsr", "%u,r%u", (unsigned)((w >> 16) & 0xF), (unsigned)F_RT(w));
        return;
    case 659:
        emit(o, "mfsrin", "r%u,r%u", (unsigned)F_RT(w), (unsigned)F_RB(w));
        return;
    case 242:
        emit(o, "mtsrin", "r%u,r%u", (unsigned)F_RT(w), (unsigned)F_RB(w));
        return;
    case 306:
        emit(o, "tlbie", "r%u", (unsigned)F_RB(w));
        return;
    case 598:
        emit(o, "sync", "");
        return;
    case 854:
        emit(o, "eieio", "");
        return;
    case 982:
    case 278:
    case 246:
    case 54:
    case 86:
    case 470:
    case 1014: {
        static const struct {
            uint16_t xo;
            const char *mnem;
        } cache[] = {
            {982,  "icbi"  },
            {278,  "dcbt"  },
            {246,  "dcbtst"},
            {54,   "dcbst" },
            {86,   "dcbf"  },
            {470,  "dcbi"  },
            {1014, "dcbz"  },
        };
        if (F_RT(w) || F_RC(w)) { // reserved fields
            invalid(o);
            return;
        }
        for (unsigned i = 0; i < sizeof(cache) / sizeof(cache[0]); i++) {
            if (cache[i].xo != xo)
                continue;
            if (F_RA(w)) // (rA|0) convention: 0 prints as 0
                emit(o, cache[i].mnem, "r%u,r%u", (unsigned)F_RA(w), (unsigned)F_RB(w));
            else
                emit(o, cache[i].mnem, "0,r%u", (unsigned)F_RB(w));
        }
        return;
    }
    case 150:
        if (F_RC(w)) {
            dis_xform_mem(o, w & ~1u, "stwcx.", 0); // Rc masked so the helper prints cleanly
            return;
        }
        invalid(o);
        return;
    case 597: { // lswi
        uint32_t nb = F_RB(w) ? F_RB(w) : 32;
        emit(o, "lswi", "r%u,r%u,%u", (unsigned)F_RT(w), (unsigned)F_RA(w), (unsigned)nb);
        return;
    }
    case 725: { // stswi
        uint32_t nb = F_RB(w) ? F_RB(w) : 32;
        emit(o, "stswi", "r%u,r%u,%u", (unsigned)F_RT(w), (unsigned)F_RA(w), (unsigned)nb);
        return;
    }
    case 371: // mftb — not a 601 instruction
    default:
        invalid(o);
        return;
    }
}

// === opcode 63 (FP double group) ===
static void dis_op63(ppc_insn *o, uint32_t w) {
    char m[16];
    switch (F_XO10(w)) {
    case 0:
        emit(o, "fcmpu", "cr%u,f%u,f%u", (unsigned)F_CRFD(w), (unsigned)F_RA(w), (unsigned)F_RB(w));
        return;
    case 32:
        emit(o, "fcmpo", "cr%u,f%u,f%u", (unsigned)F_CRFD(w), (unsigned)F_RA(w), (unsigned)F_RB(w));
        return;
    case 72:
        emit(o, suffix_rc(m, sizeof(m), "fmr", w), "f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RB(w));
        return;
    case 40:
        emit(o, suffix_rc(m, sizeof(m), "fneg", w), "f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RB(w));
        return;
    case 264:
        emit(o, suffix_rc(m, sizeof(m), "fabs", w), "f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RB(w));
        return;
    case 136:
        emit(o, suffix_rc(m, sizeof(m), "fnabs", w), "f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RB(w));
        return;
    case 583:
        emit(o, suffix_rc(m, sizeof(m), "mffs", w), "f%u", (unsigned)F_RT(w));
        return;
    case 711:
        emit(o, suffix_rc(m, sizeof(m), "mtfsf", w), "%u,f%u", (unsigned)((w >> 17) & 0xFF), (unsigned)F_RB(w));
        return;
    case 134:
        emit(o, suffix_rc(m, sizeof(m), "mtfsfi", w), "cr%u,%u", (unsigned)F_CRFD(w), (unsigned)((w >> 12) & 0xF));
        return;
    case 70:
        emit(o, suffix_rc(m, sizeof(m), "mtfsb0", w), "%u", (unsigned)F_RT(w));
        return;
    case 38:
        emit(o, suffix_rc(m, sizeof(m), "mtfsb1", w), "%u", (unsigned)F_RT(w));
        return;
    case 64:
        emit(o, "mcrfs", "cr%u,cr%u", (unsigned)F_CRFD(w), (unsigned)((w >> 18) & 7));
        return;
    case 12:
        emit(o, suffix_rc(m, sizeof(m), "frsp", w), "f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RB(w));
        return;
    case 14:
        emit(o, suffix_rc(m, sizeof(m), "fctiw", w), "f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RB(w));
        return;
    case 15:
        emit(o, suffix_rc(m, sizeof(m), "fctiwz", w), "f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RB(w));
        return;
    default:
        break;
    }
    // A-form arithmetic (5-bit XO); unused operand fields are reserved-zero
    switch ((w >> 1) & 0x1Fu) {
    case 18:
    case 20:
    case 21: {
        if ((w >> 6) & 31u) { // FRC reserved
            invalid(o);
            return;
        }
        uint32_t x5 = (w >> 1) & 0x1Fu;
        const char *base = (x5 == 18) ? "fdiv" : (x5 == 20) ? "fsub" : "fadd";
        emit(o, suffix_rc(m, sizeof(m), base, w), "f%u,f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RA(w),
             (unsigned)F_RB(w));
        return;
    }
    case 25:
        if (F_RB(w)) { // FRB reserved
            invalid(o);
            return;
        }
        emit(o, suffix_rc(m, sizeof(m), "fmul", w), "f%u,f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RA(w),
             (unsigned)((w >> 6) & 31));
        return;
    case 28:
        emit(o, suffix_rc(m, sizeof(m), "fmsub", w), "f%u,f%u,f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RA(w),
             (unsigned)((w >> 6) & 31), (unsigned)F_RB(w));
        return;
    case 29:
        emit(o, suffix_rc(m, sizeof(m), "fmadd", w), "f%u,f%u,f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RA(w),
             (unsigned)((w >> 6) & 31), (unsigned)F_RB(w));
        return;
    case 30:
        emit(o, suffix_rc(m, sizeof(m), "fnmsub", w), "f%u,f%u,f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RA(w),
             (unsigned)((w >> 6) & 31), (unsigned)F_RB(w));
        return;
    case 31:
        emit(o, suffix_rc(m, sizeof(m), "fnmadd", w), "f%u,f%u,f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RA(w),
             (unsigned)((w >> 6) & 31), (unsigned)F_RB(w));
        return;
    default:
        invalid(o);
        return;
    }
}

// === opcode 59 (FP single group; same shapes with "s" suffix) ===
static void dis_op59(ppc_insn *o, uint32_t w) {
    char m[16];
    switch ((w >> 1) & 0x1Fu) {
    case 18:
    case 20:
    case 21: {
        if ((w >> 6) & 31u) { // FRC reserved
            invalid(o);
            return;
        }
        uint32_t x5 = (w >> 1) & 0x1Fu;
        const char *base = (x5 == 18) ? "fdivs" : (x5 == 20) ? "fsubs" : "fadds";
        emit(o, suffix_rc(m, sizeof(m), base, w), "f%u,f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RA(w),
             (unsigned)F_RB(w));
        return;
    }
    case 25:
        if (F_RB(w)) { // FRB reserved
            invalid(o);
            return;
        }
        emit(o, suffix_rc(m, sizeof(m), "fmuls", w), "f%u,f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RA(w),
             (unsigned)((w >> 6) & 31));
        return;
    case 28:
        emit(o, suffix_rc(m, sizeof(m), "fmsubs", w), "f%u,f%u,f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RA(w),
             (unsigned)((w >> 6) & 31), (unsigned)F_RB(w));
        return;
    case 29:
        emit(o, suffix_rc(m, sizeof(m), "fmadds", w), "f%u,f%u,f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RA(w),
             (unsigned)((w >> 6) & 31), (unsigned)F_RB(w));
        return;
    case 30:
        emit(o, suffix_rc(m, sizeof(m), "fnmsubs", w), "f%u,f%u,f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RA(w),
             (unsigned)((w >> 6) & 31), (unsigned)F_RB(w));
        return;
    case 31:
        emit(o, suffix_rc(m, sizeof(m), "fnmadds", w), "f%u,f%u,f%u,f%u", (unsigned)F_RT(w), (unsigned)F_RA(w),
             (unsigned)((w >> 6) & 31), (unsigned)F_RB(w));
        return;
    default:
        invalid(o);
        return;
    }
}

int ppc_disassemble(uint32_t word, uint32_t addr, ppc_insn *out) {
    memset(out, 0, sizeof(*out));
    out->word = word;
    out->addr = addr;
    out->status = PPC_DIS_OK;

    uint32_t rt = F_RT(word), ra = F_RA(word);

    switch (F_OPCD(word)) {
    case 3:
        emit(out, "twi", "%u,r%u,%d", (unsigned)rt, (unsigned)ra, (int)F_SIMM(word));
        break;
    case 7:
        emit(out, "mulli", "r%u,r%u,%d", (unsigned)rt, (unsigned)ra, (int)F_SIMM(word));
        break;
    case 8:
        emit(out, "subfic", "r%u,r%u,%d", (unsigned)rt, (unsigned)ra, (int)F_SIMM(word));
        break;
    case 9:
        out->is_power = 1;
        emit(out, "dozi", "r%u,r%u,%d", (unsigned)rt, (unsigned)ra, (int)F_SIMM(word));
        break;
    case 10:
        emit(out, "cmplwi", "cr%u,r%u,%u", (unsigned)F_CRFD(word), (unsigned)ra, (unsigned)F_UIMM(word));
        break;
    case 11:
        emit(out, "cmpwi", "cr%u,r%u,%d", (unsigned)F_CRFD(word), (unsigned)ra, (int)F_SIMM(word));
        break;
    case 12:
        emit(out, "addic", "r%u,r%u,%d", (unsigned)rt, (unsigned)ra, (int)F_SIMM(word));
        break;
    case 13:
        emit(out, "addic.", "r%u,r%u,%d", (unsigned)rt, (unsigned)ra, (int)F_SIMM(word));
        break;
    case 14:
        if (ra == 0)
            emit(out, "li", "r%u,%d", (unsigned)rt, (int)F_SIMM(word));
        else
            emit(out, "addi", "r%u,r%u,%d", (unsigned)rt, (unsigned)ra, (int)F_SIMM(word));
        break;
    case 15:
        if (ra == 0)
            emit(out, "lis", "r%u,%d", (unsigned)rt, (int)F_SIMM(word));
        else
            emit(out, "addis", "r%u,r%u,%d", (unsigned)rt, (unsigned)ra, (int)F_SIMM(word));
        break;
    case 16:
        dis_bcond(out, word, addr, 0);
        break;
    case 17:
        if ((word & ~0xFC000002u) == 0 && (word & 2u)) {
            out->is_branch = 1;
            emit(out, "sc", "");
        } else {
            invalid(out);
        }
        break;
    case 18: {
        int32_t li = (int32_t)(word << 6) >> 6;
        li &= ~3;
        out->is_branch = 1;
        out->has_target = 1;
        out->target = (word & 2u) ? (uint32_t)li : addr + (uint32_t)li;
        char m[8];
        snprintf(m, sizeof(m), "b%s%s", (word & 1u) ? "l" : "", (word & 2u) ? "a" : "");
        emit(out, m, "$%08X", (unsigned)out->target);
        break;
    }
    case 19:
        dis_op19(out, word, addr);
        break;
    case 20: {
        char m[16];
        emit(out, suffix_rc(m, sizeof(m), "rlwimi", word), "r%u,r%u,%u,%u,%u", (unsigned)ra, (unsigned)rt,
             (unsigned)F_RB(word), (unsigned)F_MB(word), (unsigned)F_ME(word));
        break;
    }
    case 21: {
        char m[16];
        emit(out, suffix_rc(m, sizeof(m), "rlwinm", word), "r%u,r%u,%u,%u,%u", (unsigned)ra, (unsigned)rt,
             (unsigned)F_RB(word), (unsigned)F_MB(word), (unsigned)F_ME(word));
        break;
    }
    case 22: {
        char m[16];
        out->is_power = 1;
        emit(out, suffix_rc(m, sizeof(m), "rlmi", word), "r%u,r%u,r%u,%u,%u", (unsigned)ra, (unsigned)rt,
             (unsigned)F_RB(word), (unsigned)F_MB(word), (unsigned)F_ME(word));
        break;
    }
    case 23: {
        char m[16];
        emit(out, suffix_rc(m, sizeof(m), "rlwnm", word), "r%u,r%u,r%u,%u,%u", (unsigned)ra, (unsigned)rt,
             (unsigned)F_RB(word), (unsigned)F_MB(word), (unsigned)F_ME(word));
        break;
    }
    case 24:
        if (rt == 0 && ra == 0 && F_UIMM(word) == 0)
            emit(out, "nop", "");
        else
            emit(out, "ori", "r%u,r%u,%u", (unsigned)ra, (unsigned)rt, (unsigned)F_UIMM(word));
        break;
    case 25:
        emit(out, "oris", "r%u,r%u,%u", (unsigned)ra, (unsigned)rt, (unsigned)F_UIMM(word));
        break;
    case 26:
        emit(out, "xori", "r%u,r%u,%u", (unsigned)ra, (unsigned)rt, (unsigned)F_UIMM(word));
        break;
    case 27:
        emit(out, "xoris", "r%u,r%u,%u", (unsigned)ra, (unsigned)rt, (unsigned)F_UIMM(word));
        break;
    case 28:
        emit(out, "andi.", "r%u,r%u,%u", (unsigned)ra, (unsigned)rt, (unsigned)F_UIMM(word));
        break;
    case 29:
        emit(out, "andis.", "r%u,r%u,%u", (unsigned)ra, (unsigned)rt, (unsigned)F_UIMM(word));
        break;
    case 31:
        dis_op31(out, word, addr);
        break;
    case 32:
        dis_dform_mem(out, word, "lwz", 0);
        break;
    case 33:
        dis_dform_mem(out, word, "lwzu", 0);
        break;
    case 34:
        dis_dform_mem(out, word, "lbz", 0);
        break;
    case 35:
        dis_dform_mem(out, word, "lbzu", 0);
        break;
    case 36:
        dis_dform_mem(out, word, "stw", 0);
        break;
    case 37:
        dis_dform_mem(out, word, "stwu", 0);
        break;
    case 38:
        dis_dform_mem(out, word, "stb", 0);
        break;
    case 39:
        dis_dform_mem(out, word, "stbu", 0);
        break;
    case 40:
        dis_dform_mem(out, word, "lhz", 0);
        break;
    case 41:
        dis_dform_mem(out, word, "lhzu", 0);
        break;
    case 42:
        dis_dform_mem(out, word, "lha", 0);
        break;
    case 43:
        dis_dform_mem(out, word, "lhau", 0);
        break;
    case 44:
        dis_dform_mem(out, word, "sth", 0);
        break;
    case 45:
        dis_dform_mem(out, word, "sthu", 0);
        break;
    case 46:
        dis_dform_mem(out, word, "lmw", 0);
        break;
    case 47:
        dis_dform_mem(out, word, "stmw", 0);
        break;
    case 48:
        dis_dform_mem(out, word, "lfs", 1);
        break;
    case 49:
        dis_dform_mem(out, word, "lfsu", 1);
        break;
    case 50:
        dis_dform_mem(out, word, "lfd", 1);
        break;
    case 51:
        dis_dform_mem(out, word, "lfdu", 1);
        break;
    case 52:
        dis_dform_mem(out, word, "stfs", 1);
        break;
    case 53:
        dis_dform_mem(out, word, "stfsu", 1);
        break;
    case 54:
        dis_dform_mem(out, word, "stfd", 1);
        break;
    case 55:
        dis_dform_mem(out, word, "stfdu", 1);
        break;
    case 59:
        dis_op59(out, word);
        break;
    case 63:
        dis_op63(out, word);
        break;
    default:
        invalid(out);
        break;
    }
    return out->status;
}
