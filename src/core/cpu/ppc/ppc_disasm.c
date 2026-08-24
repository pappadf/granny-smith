// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_disasm.c
// Dependency-free MPC601/MPC604 disassembler: the second instantiation of
// the shared decode tree (ppc_decode.h), with the OP_ leaves overloaded by
// sprintf-style printing macros — the cpu_disasm.c pattern
// (proposal-heterogeneous-multi-cpu.md §3.3.1).  Because this is literally
// the same decode tree the interpreter runs, the two cannot drift.
//
// Output uses standard mnemonics with the common simplified forms (li,
// lis, mr, nop, blr, bctr, cmpwi, mflr, ...) the way the development
// oracle (powerpc-linux-gnu-objdump -m powerpc:601 / powerpc:604) prints
// them.  Model validity is flag-based (is_power / is_604) with
// ppc_disassemble_model() applying one model's view.

#include "ppc_disasm.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Field extraction (BE bit numbering) — the dependency-free copy of the
// accessor set ppc_internal.h provides to the emulator; kept in sync.
#define PPC_OPCD(iw) ((iw) >> 26)
#define PPC_RT(iw)   (((iw) >> 21) & 31)
#define PPC_RA(iw)   (((iw) >> 16) & 31)
#define PPC_RB(iw)   (((iw) >> 11) & 31)
#define PPC_XO10(iw) (((iw) >> 1) & 0x3FF)
#define PPC_XO9(iw)  (((iw) >> 1) & 0x1FF)
#define PPC_XO5(iw)  (((iw) >> 1) & 0x1F)
#define PPC_OE(iw)   (((iw) >> 10) & 1)
#define PPC_RC(iw)   ((iw) & 1)
#define PPC_SIMM(iw) ((int32_t)(int16_t)(iw))
#define PPC_UIMM(iw) ((iw) & 0xFFFFu)
#define PPC_MB(iw)   (((iw) >> 6) & 31)
#define PPC_ME(iw)   (((iw) >> 1) & 31)
#define PPC_FRC(iw)  (((iw) >> 6) & 31)
#define PPC_CRFD(iw) (((iw) >> 23) & 7)
#define PPC_CRFS(iw) (((iw) >> 18) & 7)

// === Printing helpers =======================================================

// printf into out->text ("mnemonic\toperands"; formats embed the tab).
static void emitf(ppc_insn *o, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(o->text, sizeof(o->text), fmt, ap);
    va_end(ap);
}

static void invalid(ppc_insn *o) {
    o->status = PPC_DIS_INVALID;
    snprintf(o->text, sizeof(o->text), ".long\t$%08X", (unsigned)o->word);
}

// SPR name per the 601 encoding tables (10-4/10-5) and the 604 additions
// (604UM Table 2-43 + the OEA set); NULL if unknown.
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
    case 284:
        return "tbl"; // 604 write encoding (reads via mftb)
    case 285:
        return "tbu";
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
    case 536:
        return "dbat0u"; // 604 split file
    case 537:
        return "dbat0l";
    case 538:
        return "dbat1u";
    case 539:
        return "dbat1l";
    case 540:
        return "dbat2u";
    case 541:
        return "dbat2l";
    case 542:
        return "dbat3u";
    case 543:
        return "dbat3l";
    case 952:
        return "mmcr0"; // 604 performance monitor group
    case 953:
        return "pmc1";
    case 954:
        return "pmc2";
    case 955:
        return "sia";
    case 959:
        return "sda";
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

// Conditional-branch printing shared by bc / bclr / bcctr.  `kind` is 0
// for bc (pc-relative/absolute target), 1 for lr, 2 for ctr.  BO validity
// was already decided by the decode tree.
static void dis_bcond(ppc_insn *o, uint32_t w, uint32_t addr, int kind) {
    static const char *const t_names[4] = {"lt", "gt", "eq", "so"};
    static const char *const f_names[4] = {"ge", "le", "ne", "ns"};
    uint32_t bo = PPC_RT(w), bi = PPC_RA(w);
    const char *lk = PPC_RC(w) ? "l" : "";
    const char *tail = (kind == 1) ? "lr" : (kind == 2) ? "ctr" : "";
    const char *aa = (kind == 0 && (w & 2u)) ? "a" : "";
    char mnem[24];
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
            snprintf(mnem, sizeof(mnem), "b%s%s", lk, aa);
        else
            snprintf(mnem, sizeof(mnem), "b%s%s", tail, lk);
        if (tgt[0])
            emitf(o, "%s\t%s", mnem, tgt);
        else
            emitf(o, "%s", mnem);
        return;
    }
    if ((bo & 0x1Cu) == 0x0Cu || (bo & 0x1Cu) == 0x04u) { // condition test, no CTR
        const char *cond = ((bo & 8u) ? t_names : f_names)[bi & 3u];
        snprintf(mnem, sizeof(mnem), "b%s%s%s%s", cond, tail, lk, aa);
        if (bi >= 4 && tgt[0])
            emitf(o, "%s\tcr%u,%s", mnem, bi >> 2, tgt);
        else if (bi >= 4)
            emitf(o, "%s\tcr%u", mnem, bi >> 2);
        else if (tgt[0])
            emitf(o, "%s\t%s", mnem, tgt);
        else
            emitf(o, "%s", mnem);
        return;
    }
    if (((bo & 0x16u) == 0x10u || (bo & 0x16u) == 0x12u) && bi == 0) { // CTR-only
        snprintf(mnem, sizeof(mnem), "%s%s%s%s", (bo & 2u) ? "bdz" : "bdnz", tail, lk, aa);
        if (tgt[0])
            emitf(o, "%s\t%s", mnem, tgt);
        else
            emitf(o, "%s", mnem);
        return;
    }
    // Everything else (incl. CTR+condition combinations): generic bc form
    if (tgt[0])
        emitf(o, "bc%s%s%s\t%u,%u,%s", tail, lk, aa, bo, bi, tgt);
    else
        emitf(o, "bc%s%s%s\t%u,%u", tail, lk, aa, bo, bi);
}

static void dis_b(ppc_insn *o, uint32_t w, uint32_t addr) {
    int32_t li = (int32_t)(w << 6) >> 6;
    li &= ~3;
    o->is_branch = 1;
    o->has_target = 1;
    o->target = (w & 2u) ? (uint32_t)li : addr + (uint32_t)li;
    emitf(o, "b%s%s\t$%08X", (w & 1u) ? "l" : "", (w & 2u) ? "a" : "", (unsigned)o->target);
}

// D-form load/store: "op rT,disp(rA)" with rA=0 printed as 0
static void dis_dform_mem(ppc_insn *o, uint32_t w, const char *mnem, int fp) {
    if (PPC_RA(w))
        emitf(o, "%s\t%c%u,%d(r%u)", mnem, fp ? 'f' : 'r', (unsigned)PPC_RT(w), (int)PPC_SIMM(w), (unsigned)PPC_RA(w));
    else
        emitf(o, "%s\t%c%u,%d(0)", mnem, fp ? 'f' : 'r', (unsigned)PPC_RT(w), (int)PPC_SIMM(w));
}

// X-form load/store: "opx rT,rA,rB" with rA=0 printed as 0
static void dis_xform_mem(ppc_insn *o, uint32_t w, const char *mnem, int fp) {
    if (PPC_RA(w))
        emitf(o, "%s\t%c%u,r%u,r%u", mnem, fp ? 'f' : 'r', (unsigned)PPC_RT(w), (unsigned)PPC_RA(w),
              (unsigned)PPC_RB(w));
    else
        emitf(o, "%s\t%c%u,0,r%u", mnem, fp ? 'f' : 'r', (unsigned)PPC_RT(w), (unsigned)PPC_RB(w));
}

// mfspr/mtspr with the model SPR names; the canonical short forms for the
// user SPRs (mflr/mtctr/...); MQ/RTC encodings flagged 601-specific.
static void dis_spr(ppc_insn *o, uint32_t w, int is_mf) {
    uint32_t n = ((w >> 16) & 0x1Fu) | (((w >> 11) & 0x1Fu) << 5);
    const char *name = spr_name(n);
    if (name && (n == 1 || n == 8 || n == 9)) {
        emitf(o, "%s%s\tr%u", is_mf ? "mf" : "mt", name, (unsigned)PPC_RT(w));
        return;
    }
    if (n == 0 || n == 4 || n == 5 || n == 20 || n == 21)
        o->is_power = 1; // MQ/RTC encodings are 601-specific (the 604 traps them)
    if (is_mf) {
        if (name)
            emitf(o, "mfspr\tr%u,%s", (unsigned)PPC_RT(w), name);
        else
            emitf(o, "mfspr\tr%u,%u", (unsigned)PPC_RT(w), (unsigned)n);
    } else {
        if (name)
            emitf(o, "mtspr\t%s,r%u", name, (unsigned)PPC_RT(w));
        else
            emitf(o, "mtspr\t%u,r%u", (unsigned)n, (unsigned)PPC_RT(w));
    }
}

// mftb/mftbu simplified mnemonics per the TBR number (604-only encoding).
static void dis_mftb(ppc_insn *o, uint32_t w) {
    uint32_t n = ((w >> 16) & 0x1Fu) | (((w >> 11) & 0x1Fu) << 5);
    o->is_604 = 1;
    if (n == 268)
        emitf(o, "mftb\tr%u", (unsigned)PPC_RT(w));
    else if (n == 269)
        emitf(o, "mftbu\tr%u", (unsigned)PPC_RT(w));
    else // undefined TBR: print the raw form (traps at runtime)
        emitf(o, "mftb\tr%u,%u", (unsigned)PPC_RT(w), (unsigned)n);
}

// === The OP_ printing table =================================================
// Locals `iw`, `addr`, and `o` are bound by the decoder prologue.  Suffix
// helpers read Rc/OE from the word; PWR marks the POWER holdovers.

// clang-format off

#define ASM(...)  emitf(o, __VA_ARGS__)
#define PWR       (o->is_power = 1)
#define RCS       (PPC_RC(iw) ? "." : "")
#define OES       (PPC_OE(iw) ? "o" : "")
#define RT_       ((unsigned)PPC_RT(iw))
#define RA_       ((unsigned)PPC_RA(iw))
#define RB_       ((unsigned)PPC_RB(iw))
#define CRFD_     ((unsigned)PPC_CRFD(iw))

// Operand shapes shared by the op-31 families
#define ASM_RT_RA_RB(m)  ASM(m "%s%s\tr%u,r%u,r%u", OES, RCS, RT_, RA_, RB_)
#define ASM_RT_RA(m)     ASM(m "%s%s\tr%u,r%u", OES, RCS, RT_, RA_)
#define ASM_RA_RS_RB(m)  ASM(m "%s\tr%u,r%u,r%u", RCS, RA_, RT_, RB_)
#define ASM_RA_RS_SH(m)  ASM(m "%s\tr%u,r%u,%u", RCS, RA_, RT_, RB_)
#define ASM_RA_RS(m)     ASM(m "%s\tr%u,r%u", RCS, RA_, RT_)
#define ASM_CACHE(m)     do { if (PPC_RA(iw)) ASM(m "\tr%u,r%u", RA_, RB_); else ASM(m "\t0,r%u", RB_); } while (0)
#define ASM_FRT_FRB(m)   ASM(m "%s\tf%u,f%u", RCS, RT_, RB_)
#define ASM_FARITH3(m)   ASM(m "%s\tf%u,f%u,f%u", RCS, RT_, RA_, RB_)
#define ASM_FMUL(m)      ASM(m "%s\tf%u,f%u,f%u", RCS, RT_, RA_, (unsigned)PPC_FRC(iw))
#define ASM_FMADD(m)     ASM(m "%s\tf%u,f%u,f%u,f%u", RCS, RT_, RA_, (unsigned)PPC_FRC(iw), RB_)

// --- immediates, compares, traps ---
#define OP_TWI        ASM("twi\t%u,r%u,%d", RT_, RA_, (int)PPC_SIMM(iw))
#define OP_TW         ASM("tw\t%u,r%u,r%u", RT_, RA_, RB_)
#define OP_MULLI      ASM("mulli\tr%u,r%u,%d", RT_, RA_, (int)PPC_SIMM(iw))
#define OP_SUBFIC     ASM("subfic\tr%u,r%u,%d", RT_, RA_, (int)PPC_SIMM(iw))
#define OP_DOZI       (PWR, ASM("dozi\tr%u,r%u,%d", RT_, RA_, (int)PPC_SIMM(iw)))
#define OP_CMPLI      ASM("cmplwi\tcr%u,r%u,%u", CRFD_, RA_, (unsigned)PPC_UIMM(iw))
#define OP_CMPI       ASM("cmpwi\tcr%u,r%u,%d", CRFD_, RA_, (int)PPC_SIMM(iw))
#define OP_CMPL       ASM("cmplw\tcr%u,r%u,r%u", CRFD_, RA_, RB_)
#define OP_CMP        ASM("cmpw\tcr%u,r%u,r%u", CRFD_, RA_, RB_)
#define OP_ADDIC      ASM("addic\tr%u,r%u,%d", RT_, RA_, (int)PPC_SIMM(iw))
#define OP_ADDIC_DOT  ASM("addic.\tr%u,r%u,%d", RT_, RA_, (int)PPC_SIMM(iw))
#define OP_ADDI       do { if (PPC_RA(iw)) ASM("addi\tr%u,r%u,%d", RT_, RA_, (int)PPC_SIMM(iw)); else ASM("li\tr%u,%d", RT_, (int)PPC_SIMM(iw)); } while (0)
#define OP_ADDIS      do { if (PPC_RA(iw)) ASM("addis\tr%u,r%u,%d", RT_, RA_, (int)PPC_SIMM(iw)); else ASM("lis\tr%u,%d", RT_, (int)PPC_SIMM(iw)); } while (0)

// --- branches / system ---
#define OP_BC         dis_bcond(o, iw, addr, 0)
#define OP_BCLR       dis_bcond(o, iw, addr, 1)
#define OP_BCCTR      dis_bcond(o, iw, addr, 2)
#define OP_B          dis_b(o, iw, addr)
#define OP_SC         (o->is_branch = 1, ASM("sc"))
#define OP_RFI        (o->is_branch = 1, ASM("rfi"))
#define OP_ISYNC      ASM("isync")

// --- CR logical ---
#define OP_CRAND      ASM("crand\t%u,%u,%u", RT_, RA_, RB_)
#define OP_CRANDC     ASM("crandc\t%u,%u,%u", RT_, RA_, RB_)
#define OP_CROR       ASM("cror\t%u,%u,%u", RT_, RA_, RB_)
#define OP_CRORC      ASM("crorc\t%u,%u,%u", RT_, RA_, RB_)
#define OP_CRXOR      ASM("crxor\t%u,%u,%u", RT_, RA_, RB_)
#define OP_CREQV      ASM("creqv\t%u,%u,%u", RT_, RA_, RB_)
#define OP_CRNAND     ASM("crnand\t%u,%u,%u", RT_, RA_, RB_)
#define OP_CRNOR      ASM("crnor\t%u,%u,%u", RT_, RA_, RB_)
#define OP_MCRF       ASM("mcrf\tcr%u,cr%u", CRFD_, (unsigned)PPC_CRFS(iw))

// --- rotates ---
#define OP_RLWIMI     ASM("rlwimi%s\tr%u,r%u,%u,%u,%u", RCS, RA_, RT_, RB_, (unsigned)PPC_MB(iw), (unsigned)PPC_ME(iw))
#define OP_RLWINM     ASM("rlwinm%s\tr%u,r%u,%u,%u,%u", RCS, RA_, RT_, RB_, (unsigned)PPC_MB(iw), (unsigned)PPC_ME(iw))
#define OP_RLMI       (PWR, ASM("rlmi%s\tr%u,r%u,r%u,%u,%u", RCS, RA_, RT_, RB_, (unsigned)PPC_MB(iw), (unsigned)PPC_ME(iw)))
#define OP_RLWNM      ASM("rlwnm%s\tr%u,r%u,r%u,%u,%u", RCS, RA_, RT_, RB_, (unsigned)PPC_MB(iw), (unsigned)PPC_ME(iw))

// --- logical immediates ---
#define OP_ORI        do { if (iw == 0x60000000u) ASM("nop"); else ASM("ori\tr%u,r%u,%u", RA_, RT_, (unsigned)PPC_UIMM(iw)); } while (0)
#define OP_ORIS       ASM("oris\tr%u,r%u,%u", RA_, RT_, (unsigned)PPC_UIMM(iw))
#define OP_XORI       ASM("xori\tr%u,r%u,%u", RA_, RT_, (unsigned)PPC_UIMM(iw))
#define OP_XORIS      ASM("xoris\tr%u,r%u,%u", RA_, RT_, (unsigned)PPC_UIMM(iw))
#define OP_ANDI_DOT   ASM("andi.\tr%u,r%u,%u", RA_, RT_, (unsigned)PPC_UIMM(iw))
#define OP_ANDIS_DOT  ASM("andis.\tr%u,r%u,%u", RA_, RT_, (unsigned)PPC_UIMM(iw))

// --- XO-form arithmetic ---
#define OP_ADD        ASM_RT_RA_RB("add")
#define OP_ADDC       ASM_RT_RA_RB("addc")
#define OP_ADDE       ASM_RT_RA_RB("adde")
#define OP_ADDME      ASM_RT_RA("addme")
#define OP_ADDZE      ASM_RT_RA("addze")
#define OP_SUBF       ASM_RT_RA_RB("subf")
#define OP_SUBFC      ASM_RT_RA_RB("subfc")
#define OP_SUBFE      ASM_RT_RA_RB("subfe")
#define OP_SUBFME     ASM_RT_RA("subfme")
#define OP_SUBFZE     ASM_RT_RA("subfze")
#define OP_NEG        ASM_RT_RA("neg")
#define OP_MULHW      ASM("mulhw%s\tr%u,r%u,r%u", RCS, RT_, RA_, RB_)
#define OP_MULHWU     ASM("mulhwu%s\tr%u,r%u,r%u", RCS, RT_, RA_, RB_)
#define OP_MULLW      ASM_RT_RA_RB("mullw")
#define OP_DIVW       ASM_RT_RA_RB("divw")
#define OP_DIVWU      ASM_RT_RA_RB("divwu")

// --- POWER arithmetic holdovers ---
#define OP_ABS        (PWR, ASM_RT_RA("abs"))
#define OP_NABS       (PWR, ASM_RT_RA("nabs"))
#define OP_DOZ        (PWR, ASM_RT_RA_RB("doz"))
#define OP_MUL        (PWR, ASM_RT_RA_RB("mul"))
#define OP_DIV        (PWR, ASM_RT_RA_RB("div"))
#define OP_DIVS       (PWR, ASM_RT_RA_RB("divs"))
#define OP_CLCS       (PWR, ASM("clcs%s\tr%u,r%u", RCS, RT_, RA_))

// --- logical ---
#define OP_AND        ASM_RA_RS_RB("and")
#define OP_ANDC       ASM_RA_RS_RB("andc")
#define OP_OR         do { if (PPC_RT(iw) == PPC_RB(iw) && !PPC_RC(iw)) ASM("mr\tr%u,r%u", RA_, RT_); else ASM_RA_RS_RB("or"); } while (0)
#define OP_ORC        ASM_RA_RS_RB("orc")
#define OP_XOR        ASM_RA_RS_RB("xor")
#define OP_NAND       ASM_RA_RS_RB("nand")
#define OP_NOR        ASM_RA_RS_RB("nor")
#define OP_EQV        ASM_RA_RS_RB("eqv")
#define OP_EXTSB      ASM_RA_RS("extsb")
#define OP_EXTSH      ASM_RA_RS("extsh")
#define OP_CNTLZW     ASM_RA_RS("cntlzw")

// --- architectural shifts ---
#define OP_SLW        ASM_RA_RS_RB("slw")
#define OP_SRW        ASM_RA_RS_RB("srw")
#define OP_SRAW       ASM_RA_RS_RB("sraw")
#define OP_SRAWI      ASM_RA_RS_SH("srawi")

// --- POWER shift-with-MQ family ---
#define OP_MASKG      (PWR, ASM_RA_RS_RB("maskg"))
#define OP_MASKIR     (PWR, ASM_RA_RS_RB("maskir"))
#define OP_RRIB       (PWR, ASM_RA_RS_RB("rrib"))
#define OP_SLE        (PWR, ASM_RA_RS_RB("sle"))
#define OP_SLEQ       (PWR, ASM_RA_RS_RB("sleq"))
#define OP_SLIQ       (PWR, ASM_RA_RS_SH("sliq"))
#define OP_SLLIQ      (PWR, ASM_RA_RS_SH("slliq"))
#define OP_SLLQ       (PWR, ASM_RA_RS_RB("sllq"))
#define OP_SLQ        (PWR, ASM_RA_RS_RB("slq"))
#define OP_SRQ        (PWR, ASM_RA_RS_RB("srq"))
#define OP_SRE        (PWR, ASM_RA_RS_RB("sre"))
#define OP_SRIQ       (PWR, ASM_RA_RS_SH("sriq"))
#define OP_SRLQ       (PWR, ASM_RA_RS_RB("srlq"))
#define OP_SREQ       (PWR, ASM_RA_RS_RB("sreq"))
#define OP_SRLIQ      (PWR, ASM_RA_RS_SH("srliq"))
#define OP_SRAQ       (PWR, ASM_RA_RS_RB("sraq"))
#define OP_SRAIQ      (PWR, ASM_RA_RS_SH("sraiq"))
#define OP_SREA       (PWR, ASM_RA_RS_RB("srea"))

// --- CR / MSR / SPR / SR moves ---
#define OP_MFCR       ASM("mfcr\tr%u", RT_)
#define OP_MTCRF      ASM("mtcrf\t%u,r%u", (unsigned)((iw >> 12) & 0xFF), RT_)
#define OP_MCRXR      ASM("mcrxr\tcr%u", CRFD_)
#define OP_MFSPR      dis_spr(o, iw, 1)
#define OP_MTSPR      dis_spr(o, iw, 0)
#define OP_MFMSR      ASM("mfmsr\tr%u", RT_)
#define OP_MTMSR      ASM("mtmsr\tr%u", RT_)
#define OP_MFSR       ASM("mfsr\tr%u,%u", RT_, (unsigned)((iw >> 16) & 0xF))
#define OP_MTSR       ASM("mtsr\t%u,r%u", (unsigned)((iw >> 16) & 0xF), RT_)
#define OP_MFSRIN     ASM("mfsrin\tr%u,r%u", RT_, RB_)
#define OP_MTSRIN     ASM("mtsrin\tr%u,r%u", RT_, RB_)
#define OP_TLBIE      ASM("tlbie\tr%u", RB_)
#define OP_MFTB       dis_mftb(o, iw)
#define OP_TLBSYNC    (o->is_604 = 1, ASM("tlbsync"))

// --- storage control ---
#define OP_SYNC       ASM("sync")
#define OP_EIEIO      ASM("eieio")
#define OP_ICBI       ASM_CACHE("icbi")
#define OP_DCBT       ASM_CACHE("dcbt")
#define OP_DCBTST     ASM_CACHE("dcbtst")
#define OP_DCBST      ASM_CACHE("dcbst")
#define OP_DCBF       ASM_CACHE("dcbf")
#define OP_DCBI       ASM_CACHE("dcbi")
#define OP_DCBZ       ASM_CACHE("dcbz")

// --- D-form loads/stores ---
#define OP_LWZ        dis_dform_mem(o, iw, "lwz", 0)
#define OP_LWZU       dis_dform_mem(o, iw, "lwzu", 0)
#define OP_LBZ        dis_dform_mem(o, iw, "lbz", 0)
#define OP_LBZU       dis_dform_mem(o, iw, "lbzu", 0)
#define OP_STW        dis_dform_mem(o, iw, "stw", 0)
#define OP_STWU       dis_dform_mem(o, iw, "stwu", 0)
#define OP_STB        dis_dform_mem(o, iw, "stb", 0)
#define OP_STBU       dis_dform_mem(o, iw, "stbu", 0)
#define OP_LHZ        dis_dform_mem(o, iw, "lhz", 0)
#define OP_LHZU       dis_dform_mem(o, iw, "lhzu", 0)
#define OP_LHA        dis_dform_mem(o, iw, "lha", 0)
#define OP_LHAU       dis_dform_mem(o, iw, "lhau", 0)
#define OP_STH        dis_dform_mem(o, iw, "sth", 0)
#define OP_STHU       dis_dform_mem(o, iw, "sthu", 0)
#define OP_LMW        dis_dform_mem(o, iw, "lmw", 0)
#define OP_STMW       dis_dform_mem(o, iw, "stmw", 0)
#define OP_LFS        dis_dform_mem(o, iw, "lfs", 1)
#define OP_LFSU       dis_dform_mem(o, iw, "lfsu", 1)
#define OP_LFD        dis_dform_mem(o, iw, "lfd", 1)
#define OP_LFDU       dis_dform_mem(o, iw, "lfdu", 1)
#define OP_STFS       dis_dform_mem(o, iw, "stfs", 1)
#define OP_STFSU      dis_dform_mem(o, iw, "stfsu", 1)
#define OP_STFD       dis_dform_mem(o, iw, "stfd", 1)
#define OP_STFDU      dis_dform_mem(o, iw, "stfdu", 1)

// --- indexed loads/stores ---
#define OP_LWZX       dis_xform_mem(o, iw, "lwzx", 0)
#define OP_LWZUX      dis_xform_mem(o, iw, "lwzux", 0)
#define OP_LBZX       dis_xform_mem(o, iw, "lbzx", 0)
#define OP_LBZUX      dis_xform_mem(o, iw, "lbzux", 0)
#define OP_LHZX       dis_xform_mem(o, iw, "lhzx", 0)
#define OP_LHZUX      dis_xform_mem(o, iw, "lhzux", 0)
#define OP_LHAX       dis_xform_mem(o, iw, "lhax", 0)
#define OP_LHAUX      dis_xform_mem(o, iw, "lhaux", 0)
#define OP_STWX       dis_xform_mem(o, iw, "stwx", 0)
#define OP_STWUX      dis_xform_mem(o, iw, "stwux", 0)
#define OP_STBX       dis_xform_mem(o, iw, "stbx", 0)
#define OP_STBUX      dis_xform_mem(o, iw, "stbux", 0)
#define OP_STHX       dis_xform_mem(o, iw, "sthx", 0)
#define OP_STHUX      dis_xform_mem(o, iw, "sthux", 0)
#define OP_LWBRX      dis_xform_mem(o, iw, "lwbrx", 0)
#define OP_LHBRX      dis_xform_mem(o, iw, "lhbrx", 0)
#define OP_STWBRX     dis_xform_mem(o, iw, "stwbrx", 0)
#define OP_STHBRX     dis_xform_mem(o, iw, "sthbrx", 0)
#define OP_LWARX      dis_xform_mem(o, iw, "lwarx", 0)
#define OP_STWCX_DOT  dis_xform_mem(o, iw, "stwcx.", 0)
#define OP_ECIWX      dis_xform_mem(o, iw, "eciwx", 0)
#define OP_ECOWX      dis_xform_mem(o, iw, "ecowx", 0)
#define OP_LSWX       dis_xform_mem(o, iw, "lswx", 0)
#define OP_STSWX      dis_xform_mem(o, iw, "stswx", 0)
#define OP_LSCBX      (PWR, dis_xform_mem(o, iw, PPC_RC(iw) ? "lscbx." : "lscbx", 0))
#define OP_LSWI       ASM("lswi\tr%u,r%u,%u", RT_, RA_, (unsigned)(PPC_RB(iw) ? PPC_RB(iw) : 32))
#define OP_STSWI      ASM("stswi\tr%u,r%u,%u", RT_, RA_, (unsigned)(PPC_RB(iw) ? PPC_RB(iw) : 32))
#define OP_LFSX       dis_xform_mem(o, iw, "lfsx", 1)
#define OP_LFSUX      dis_xform_mem(o, iw, "lfsux", 1)
#define OP_LFDX       dis_xform_mem(o, iw, "lfdx", 1)
#define OP_LFDUX      dis_xform_mem(o, iw, "lfdux", 1)
#define OP_STFSX      dis_xform_mem(o, iw, "stfsx", 1)
#define OP_STFSUX     dis_xform_mem(o, iw, "stfsux", 1)
#define OP_STFDX      dis_xform_mem(o, iw, "stfdx", 1)
#define OP_STFDUX     dis_xform_mem(o, iw, "stfdux", 1)
#define OP_STFIWX     (o->is_604 = 1, dis_xform_mem(o, iw, "stfiwx", 1))

// --- FP moves / FPSCR / compares ---
#define OP_FCMPU      ASM("fcmpu\tcr%u,f%u,f%u", CRFD_, RA_, RB_)
#define OP_FCMPO      ASM("fcmpo\tcr%u,f%u,f%u", CRFD_, RA_, RB_)
#define OP_FMR        ASM_FRT_FRB("fmr")
#define OP_FNEG       ASM_FRT_FRB("fneg")
#define OP_FABS       ASM_FRT_FRB("fabs")
#define OP_FNABS      ASM_FRT_FRB("fnabs")
#define OP_MFFS       ASM("mffs%s\tf%u", RCS, RT_)
#define OP_MTFSF      ASM("mtfsf%s\t%u,f%u", RCS, (unsigned)((iw >> 17) & 0xFF), RB_)
#define OP_MTFSFI     ASM("mtfsfi%s\tcr%u,%u", RCS, CRFD_, (unsigned)((iw >> 12) & 0xF))
#define OP_MTFSB0     ASM("mtfsb0%s\t%u", RCS, RT_)
#define OP_MTFSB1     ASM("mtfsb1%s\t%u", RCS, RT_)
#define OP_MCRFS      ASM("mcrfs\tcr%u,cr%u", CRFD_, (unsigned)PPC_CRFS(iw))

// --- FP arithmetic ---
#define OP_FRSP       ASM_FRT_FRB("frsp")
#define OP_FCTIW      ASM_FRT_FRB("fctiw")
#define OP_FCTIWZ     ASM_FRT_FRB("fctiwz")
#define OP_FDIV       ASM_FARITH3("fdiv")
#define OP_FSUB       ASM_FARITH3("fsub")
#define OP_FADD       ASM_FARITH3("fadd")
#define OP_FMUL       ASM_FMUL("fmul")
#define OP_FMSUB      ASM_FMADD("fmsub")
#define OP_FMADD      ASM_FMADD("fmadd")
#define OP_FNMSUB     ASM_FMADD("fnmsub")
#define OP_FNMADD     ASM_FMADD("fnmadd")
#define OP_FDIVS      ASM_FARITH3("fdivs")
#define OP_FSUBS      ASM_FARITH3("fsubs")
#define OP_FADDS      ASM_FARITH3("fadds")
#define OP_FMULS      ASM_FMUL("fmuls")
#define OP_FMSUBS     ASM_FMADD("fmsubs")
#define OP_FMADDS     ASM_FMADD("fmadds")
#define OP_FNMSUBS    ASM_FMADD("fnmsubs")
#define OP_FNMADDS    ASM_FMADD("fnmadds")

// --- 604 optional-FP group ---
#define OP_FSEL       (o->is_604 = 1, ASM_FMADD("fsel"))
#define OP_FRES       (o->is_604 = 1, ASM_FRT_FRB("fres"))
#define OP_FRSQRTE    (o->is_604 = 1, ASM_FRT_FRB("frsqrte"))

#define OP_ILLEGAL    invalid(o)

// clang-format on

// === The generated decoder ==================================================

#define PPC_DECODER_NAME        ppc_disasm_one
#define PPC_DECODER_RETURN_TYPE static void
#define PPC_DECODER_ARGS        uint32_t iw, uint32_t addr, ppc_insn *o
#define PPC_DECODER_PROLOGUE    (void)addr
#define PPC_DECODER_EPILOGUE    (void)0

#include "ppc_decode.h"

int ppc_disassemble(uint32_t word, uint32_t addr, ppc_insn *out) {
    memset(out, 0, sizeof(*out));
    out->word = word;
    out->addr = addr;
    out->status = PPC_DIS_OK;
    ppc_disasm_one(word, addr, out);
    return out->status;
}

int ppc_disassemble_model(uint32_t word, uint32_t addr, int model, ppc_insn *out) {
    ppc_disassemble(word, addr, out);
    // The other model's exclusives trap as illegal there — render them
    // the way any invalid word renders.
    if ((model == 604 && out->is_power) || (model == 601 && out->is_604))
        invalid(out);
    return out->status;
}
