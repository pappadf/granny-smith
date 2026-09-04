// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

/*
 * dsp3210_dis.c — reference disassembler for the AT&T DSP3210
 *
 * See dsp3210_dis.h for provenance.  Bit-field references below are to the
 * DSP3210 Information Manual, Tables 10-1 (instruction encodings), 10-2
 * (CA field encodings) and 10-3 (DA field encodings).
 */

#include "dsp3210_disasm.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* small bounded string builder                                        */

typedef struct {
    char *buf;
    size_t len; /* bytes used, excluding nul */
    size_t cap;
} sb;

static void sb_init(sb *s, char *buf, size_t cap) {
    s->buf = buf;
    s->len = 0;
    s->cap = cap;
    if (cap)
        buf[0] = '\0';
}

static void sb_puts(sb *s, const char *str) {
    size_t n = strlen(str);
    if (s->len + n >= s->cap)
        n = (s->cap > s->len + 1) ? s->cap - s->len - 1 : 0;
    memcpy(s->buf + s->len, str, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

static void sb_fmt(sb *s, const char *fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    sb_puts(s, tmp);
}

/* ------------------------------------------------------------------ */
/* field extraction helpers                                            */

static uint32_t bits(uint32_t w, int hi, int lo) {
    return (w >> lo) & ((hi - lo == 31) ? 0xFFFFFFFFu : ((1u << (hi - lo + 1)) - 1u));
}

static int32_t sext16(uint32_t v) {
    return (int32_t)(int16_t)(v & 0xFFFFu);
}

/* ------------------------------------------------------------------ */
/* name tables (IM Table 10-2)                                         */

/*
 * The 5-bit CAU register field is deliberately discontinuous: pc occupies
 * code 15, breaking r1-r14 (codes 1-14) away from r15-r19 (codes 17-21);
 * codes 22/23 are the -n/+n pseudo-operands; r20-r22 are codes 24-26;
 * pcsh is code 30.  Codes 16, 27-29 and 31 are reserved.
 */
#define RC_R0    0
#define RC_PC    15
#define RC_MINUS 22 /* -n pseudo-operand */
#define RC_PLUS  23 /* +n pseudo-operand */
#define RC_SP    25 /* r21 */
#define RC_PCSH  30

static const char *const ca_reg_name[32] = {
    "r0",   "r1",  "r2",  "r3",  "r4",  "r5",  "r6", "r7", "r8",  "r9",  "r10", "r11",  "r12",  "r13",  "r14",  "pc",
    "?r16", "r15", "r16", "r17", "r18", "r19", "-n", "+n", "r20", "r21", "r22", "?r27", "?r28", "?r29", "pcsh", "?r31"};

/* IO registers reachable by format 7b/7d moves (all others are MMIO). */
static const char *ior_name(unsigned code, char *tmp, size_t cap) {
    switch (code) {
    case 0:
        return "ps";
    case 8:
        return "emr";
    case 10:
        return "spc";
    case 12:
        return "pcw";
    case 14:
        return "dauc";
    case 15:
        return "ctr";
    default:
        snprintf(tmp, cap, "?ior%u", code);
        return tmp;
    }
}

/* 6-bit condition field (shared by branches and conditional ALU ops). */
static const char *const cond_name_tab[64] = {
    /* 00xxxx — CAU */
    "false", "true", "pl", "mi", "ne", "eq", "vc", "vs", "cc", "cs", "ge", "lt", "gt", "le", "hi", "ls",
    /* 01xxxx — DAU */
    "auc", "aus", "age", "alt", "ane", "aeq", "avc", "avs", "agt", "ale", 0, 0, 0, 0, 0, 0,
    /* 10xxxx — I/O */
    "ibe", "ibf", "obf", "obe", 0, 0, 0, 0, "syc", "sys", "fbc", "fbs", "ir0c", "ir0s", "ir1c", "ir1s",
    /* 11xxxx — reserved */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static const char *cond_name(unsigned c, char *tmp, size_t cap) {
    const char *n = cond_name_tab[c & 63];
    if (n)
        return n;
    snprintf(tmp, cap, "?cond%02x", c & 63);
    return tmp;
}

/* Move-size keyword from the 3-bit W field (long, the default, prints as
 * nothing to match assembler convention). */
static const char *w_keyword(unsigned w) {
    switch (w & 7) {
    case 0:
        return "(byte) ";
    case 1:
        return "(char) ";
    case 2:
        return "(ushort) ";
    case 3:
        return "(short) ";
    case 4:
        return "(hbyte) ";
    case 7:
        return ""; /* (long) */
    default:
        return "(?size) "; /* 101/110 reserved */
    }
}

/* ------------------------------------------------------------------ */
/* operand printers                                                    */

/* signed 16-bit displacement: "+0x12" or "-0x12" */
static void sb_disp(sb *s, int32_t n) {
    if (n < 0)
        sb_fmt(s, "-0x%x", (unsigned)(-n));
    else
        sb_fmt(s, "+0x%x", (unsigned)n);
}

/* signed 16-bit immediate as a standalone value */
static void sb_imm(sb *s, int32_t n) {
    if (n < 0)
        sb_fmt(s, "-0x%x", (unsigned)(-n));
    else
        sb_fmt(s, "0x%x", (unsigned)n);
}

/*
 * Branch/call target "{N, rB, rB+N}" (IM GOTO/CALL pages):
 *   rB = r0  -> absolute address N
 *   rB = pc  -> pc-relative; resolved target = addr + 8 + N
 *   N  = 0   -> plain rB
 */
static void ca_target(sb *s, dsp3210_insn *ins, unsigned rb, int32_t n) {
    if (rb == RC_R0) {
        sb_fmt(s, "0x%x", (unsigned)n & 0xFFFFFFFFu);
        ins->has_target = 1;
        ins->target = (uint32_t)n;
    } else if (rb == RC_PC) {
        sb_puts(s, "pc");
        if (n != 0)
            sb_disp(s, n);
        ins->has_target = 1;
        ins->target = ins->addr + 8 + (uint32_t)n;
    } else {
        sb_puts(s, ca_reg_name[rb]);
        if (n != 0)
            sb_disp(s, n);
    }
}

/* register-indirect memory operand of the format 7c/7d moves */
static void ca_mem(sb *s, unsigned rp, unsigned ri) {
    sb_fmt(s, "*%s", ca_reg_name[rp]);
    if (ri == RC_R0)
        return; /* post-modify by zero: plain *rP */
    if (ri == RC_PLUS)
        sb_puts(s, "++");
    else if (ri == RC_MINUS)
        sb_puts(s, "--");
    else
        sb_fmt(s, "++%s", ca_reg_name[ri]);
}

/*
 * 7-bit DA X/Y/Z field: p (bits 6-3) selects a pointer register r1-r14;
 * i (bits 2-0) the post-modification (0 = none, 1-5 = r15-r19, 6/7 =
 * -/+ operand size).  p = 0 is register-direct: i = 0-3 selects a0-a3
 * (X/Y only), i = 7 means "no write" (Z only).  p = 15 is not allowed.
 *
 * Returns 0 if the field is a reserved/not-allowed combination.
 */
static int da_operand(sb *s, unsigned f, int is_z) {
    unsigned p = (f >> 3) & 15, i = f & 7;

    if (p == 0) {
        if (!is_z && i <= 3) {
            sb_fmt(s, "a%u", i);
            return 1;
        }
        sb_puts(s, "?");
        return 0; /* "no write" Z (i==7) is handled by the caller */
    }
    if (p == 15) {
        sb_puts(s, "?");
        return 0;
    }
    sb_fmt(s, "*r%u", p);
    switch (i) {
    case 0:
        break;
    case 6:
        sb_puts(s, "--");
        break;
    case 7:
        sb_puts(s, "++");
        break;
    default:
        sb_fmt(s, "++r%u", 14 + i);
        break; /* 1-5 -> r15-r19 */
    }
    return 1;
}

/*
 * "No write" Z encodings: the manual's examples use p=0000,i=111 (0x07),
 * but Apple's assembler output (ROM '3210' segments, enabler dspf
 * modules) uses p=1111,i=111 (0x7F).  Since p=1111 cannot address
 * memory, treat i=111 with a non-pointer p as "no write" as well.
 */
static int da_z_is_write(unsigned z) {
    unsigned p = (z >> 3) & 15, i = z & 7;
    if (i == 7 && (p == 0 || p == 15))
        return 0;
    return p != 0;
}

/* ------------------------------------------------------------------ */
/* DA (floating point) decode                                          */

static const char *const da_gfunc[16] = {"ic",      "oc",    "float16", "int16", "round", "ifalt", "ifaeq", "ifagt",
                                         "float32", "int32", 0,         0,       "ieee",  "dsp",   "seed",  0};

static void dis_da(uint32_t w, dsp3210_insn *ins, sb *s) {
    unsigned fmt = bits(w, 31, 29); /* 1, 2 or 3 */
    unsigned m = bits(w, 28, 26);
    unsigned n = bits(w, 22, 21);
    unsigned x = bits(w, 20, 14);
    unsigned y = bits(w, 13, 7);
    unsigned z = bits(w, 6, 0);
    unsigned fs = bits(w, 24, 24); /* adder-input sign  (0=+ 1=-) */
    unsigned ss = bits(w, 23, 23); /* product sign      (0=+ 1=-) */
    int zw = da_z_is_write(z);
    int ok = 1;

    ins->klass = DSP3210_CLASS_DA;

    /* Z with p=1111: undocumented "through Y" spelling ([IM] calls
     * p=1111 not allowed; Apple's assembler emits it throughout the AV
     * sound modules).  The store goes through the Y operand's address
     * and Z's I field post-modifies Y's pointer register — render it as
     * that pointer.  With an accumulator Y there is nothing to store
     * through, which is where the old "p=1111,i=111 = no write" reading
     * came from; treat any I as no write there. */
    if (((z >> 3) & 15) == 15) {
        unsigned yp = (y >> 3) & 15;
        z = (yp != 0 && yp != 15) ? ((yp << 3) | (z & 7)) : 0x07u;
        zw = da_z_is_write(z);
    }

    /* Format 5 — special functions: [Z =] aN = g(Y)  (031111 GGGG NN) */
    if (fmt == 3 && m >= 6) {
        unsigned g = bits(w, 26, 23);
        const char *gn = da_gfunc[g];

        n = bits(w, 22, 21);
        if (zw) {
            ok &= da_operand(s, z, 1);
            sb_puts(s, " = ");
        }
        sb_fmt(s, "a%u = ", n);
        if (gn)
            sb_puts(s, gn);
        else {
            sb_fmt(s, "?gfunc%x", g);
            ins->status = DSP3210_RESERVED;
        }
        sb_puts(s, "(");
        ok &= da_operand(s, y, 0);
        sb_puts(s, ")");
        if (!ok && ins->status == DSP3210_OK)
            ins->status = DSP3210_RESERVED;
        return;
    }

    /*
     * Multiply/accumulate formats (IM Table 10-1, instruction pages):
     *   fmt 1, M=aM :  [Z =] aN = [-]Y {+,-} aM * X     FMULT-ADD-STORE
     *   fmt 1, M=1.0:  [Z =] aN = [-]Y {+,-} X          FADD-STORE
     *   fmt 1, M=0.0:  [Z =] aN = [-]Y                  FLOAD-STORE
     *   fmt 1, M=110:  aN = [-](Z=Y) {+,-} X            FADD-TAP (fmt "4")
     *   fmt 2, M=aM :  aN = [-]aM {+,-} (Z=Y) * X       FMULT-ACC-TAP
     *   fmt 2, M=0.0:  aN = [-](Z=Y) * X                FMULT-TAP
     *   fmt 3, M=aM :  [Z =] aN = [-]aM {+,-} Y * X     FMULT-ACC-STORE
     *   fmt 3, M=0.0:  [Z =] aN = [-]Y * X              FMULT-STORE
     * F is the sign written before the adder operand, S the sign of the
     * product term.  M = 100 -> 0.0, 101 -> 1.0.
     */
    {
        int tap = (fmt == 2) || (fmt == 1 && m == 6); /* Z=Y forms */

        if (zw && !tap) {
            ok &= da_operand(s, z, 1);
            sb_puts(s, " = ");
        }
        sb_fmt(s, "a%u = ", n);

        if (fmt == 1 && m == 6) {
            /* FADD-TAP: aN = [-](Z=Y) {+,-} X */
            if (fs)
                sb_puts(s, "-");
            if (zw) {
                sb_puts(s, "(");
                ok &= da_operand(s, z, 1);
                sb_puts(s, " = ");
                ok &= da_operand(s, y, 0);
                sb_puts(s, ")");
            } else {
                ok &= da_operand(s, y, 0);
            }
            sb_puts(s, ss ? " - " : " + ");
            ok &= da_operand(s, x, 0);
        } else if (fmt == 1) {
            /* adder input = Y; multiplier = {aM, 0.0, 1.0} * X */
            if (fs)
                sb_puts(s, "-");
            ok &= da_operand(s, y, 0);
            if (m == 4) {
                /* 0.0 * X — product vanishes; show it only if X != a0 */
                if (x != 0) {
                    sb_puts(s, ss ? " - " : " + ");
                    sb_puts(s, "0.0 * ");
                    ok &= da_operand(s, x, 0);
                }
            } else {
                sb_puts(s, ss ? " - " : " + ");
                if (m == 5)
                    ; /* 1.0 * X prints as just X */
                else
                    sb_fmt(s, "a%u * ", m);
                ok &= da_operand(s, x, 0);
            }
        } else {
            /* fmt 2 (tap) and fmt 3 (store): product = Y * X,
             * adder input = {aM, 0.0, 1.0} */
            int have_adder = 1;

            if (m == 4)
                have_adder = 0; /* + 0.0 is implicit */
            if (have_adder) {
                if (fs)
                    sb_puts(s, "-");
                if (m == 5)
                    sb_puts(s, "1.0");
                else
                    sb_fmt(s, "a%u", m);
                sb_puts(s, ss ? " - " : " + ");
            } else {
                if (fs) {
                    /* -0.0 as adder input: keep it visible */
                    sb_puts(s, ss ? "-0.0 - " : "-0.0 + ");
                } else if (ss) {
                    sb_puts(s, "-");
                }
            }
            if (fmt == 2 && zw) {
                sb_puts(s, "(");
                ok &= da_operand(s, z, 1);
                sb_puts(s, " = ");
                ok &= da_operand(s, y, 0);
                sb_puts(s, ")");
            } else {
                ok &= da_operand(s, y, 0);
            }
            sb_puts(s, " * ");
            ok &= da_operand(s, x, 0);
        }
        if (!ok && ins->status == DSP3210_OK)
            ins->status = DSP3210_RESERVED;
    }
}

/* ------------------------------------------------------------------ */
/* CA ALU (formats 6a-6d)                                              */

/* F field, IM Table 10-2 "CA - F Field" */
enum {
    F_ADD = 0,
    F_SHL_N = 1,
    F_RSUB = 2,
    F_CRADD = 3,
    F_SUB = 4,
    F_RES5 = 5,
    F_ANDC = 6,
    F_CMP = 7,
    F_XOR = 8,
    F_ROR = 9,
    F_OR = 10,
    F_ROL = 11,
    F_SHR_N = 12,
    F_ASR_N = 13,
    F_AND = 14,
    F_BTST = 15
};

static const char *const f_op_sym[16] = {
    "+", "<<", 0 /* reversed - */, "#", "-", 0, "&~", "-", "^", ">>>1", "|", "<<<1", ">>", "$>>", "&", "&"};

static void dis_ca_alu_reg(uint32_t w, dsp3210_insn *ins, sb *s) {
    unsigned e = bits(w, 31, 31); /* 0 = short, 1 = long */
    unsigned f = bits(w, 24, 21);
    unsigned rd = bits(w, 20, 16);
    unsigned rs1 = bits(w, 15, 11);
    unsigned c = bits(w, 10, 5);
    unsigned rs2 = bits(w, 4, 0);
    const char *size = e ? "" : "(short) ";
    char tmp[16];

    if (f == F_RES5) {
        ins->status = DSP3210_RESERVED;
        sb_fmt(s, ".word 0x%08x ; reserved ALU function", w);
        return;
    }

    /* condition prefix (C = 000001 "true" prints nothing) */
    if (c != 1)
        sb_fmt(s, "if (%s) ", cond_name(c, tmp, sizeof tmp));

    /* no-store forms */
    if (f == F_CMP || f == F_BTST) {
        sb_fmt(s, "%s%s %s %s", size, ca_reg_name[rs1], (f == F_CMP) ? "-" : "&", ca_reg_name[rs2]);
        return;
    }

    /* stack-pointer bump: sp = sp{++,--} moves by 4, not 1 (IM INCR/DECR) */
    if (f == F_ADD && rd == RC_SP && rs1 == RC_SP && (rs2 == RC_PLUS || rs2 == RC_MINUS)) {
        sb_fmt(s, "sp = %ssp%s", size, rs2 == RC_PLUS ? "++" : "--");
        return;
    }

    sb_fmt(s, "%s = %s", ca_reg_name[rd], size);

    switch (f) {
    case F_ADD:
        if (rs2 == RC_PLUS || rs2 == RC_MINUS) {
            sb_fmt(s, "%s %c 1", ca_reg_name[rs1], rs2 == RC_PLUS ? '+' : '-');
        } else if (rs1 == rs2 && rs1 != RC_R0) {
            sb_fmt(s, "%s * 2", ca_reg_name[rs1]);
        } else if (rs2 == RC_R0) {
            sb_puts(s, ca_reg_name[rs1]); /* assignment */
        } else if (rs1 == RC_R0) {
            sb_puts(s, ca_reg_name[rs2]); /* assignment */
        } else {
            sb_fmt(s, "%s + %s", ca_reg_name[rs1], ca_reg_name[rs2]);
        }
        break;
    case F_SUB:
        if (rs1 == RC_R0)
            sb_fmt(s, "-%s", ca_reg_name[rs2]); /* negate */
        else if (rs2 == RC_PLUS)
            sb_fmt(s, "%s - 1", ca_reg_name[rs1]);
        else if (rs2 == RC_MINUS)
            sb_fmt(s, "%s + 1", ca_reg_name[rs1]);
        else
            sb_fmt(s, "%s - %s", ca_reg_name[rs1], ca_reg_name[rs2]);
        break;
    case F_RSUB:
        /* immediate form is N - rD; register form is not in the
         * assembler's repertoire — render the raw meaning */
        sb_fmt(s, "%s - %s", ca_reg_name[rs2], ca_reg_name[rs1]);
        break;
    case F_ROR:
    case F_ROL:
        sb_fmt(s, "%s %s", ca_reg_name[rs1], f_op_sym[f]);
        break;
    case F_SHL_N:
    case F_SHR_N:
    case F_ASR_N:
        if (rs2 == RC_PLUS)
            sb_fmt(s, "%s %s 1", ca_reg_name[rs1], f_op_sym[f]);
        else
            sb_fmt(s, "%s %s %s", ca_reg_name[rs1], f_op_sym[f], ca_reg_name[rs2]);
        break;
    default: /* #, &~, ^, |, & */
        sb_fmt(s, "%s %s %s", ca_reg_name[rs1], f_op_sym[f], ca_reg_name[rs2]);
        break;
    }
}

static void dis_ca_alu_imm(uint32_t w, dsp3210_insn *ins, sb *s) {
    unsigned e = bits(w, 31, 31);
    unsigned f = bits(w, 24, 21);
    unsigned rd = bits(w, 20, 16);
    int32_t n = sext16(w);
    const char *size = e ? "" : "(short) ";

    if (f == F_RES5) {
        ins->status = DSP3210_RESERVED;
        sb_fmt(s, ".word 0x%08x ; reserved ALU function", w);
        return;
    }

    /* no-store forms */
    if (f == F_CMP || f == F_BTST) {
        sb_fmt(s, "%s%s %s ", size, ca_reg_name[rd], (f == F_CMP) ? "-" : "&");
        sb_imm(s, n);
        return;
    }

    sb_fmt(s, "%s = %s", ca_reg_name[rd], size);

    switch (f) {
    case F_RSUB: /* rD = N - rD */
        sb_imm(s, n);
        sb_fmt(s, " - %s", ca_reg_name[rd]);
        break;
    case F_SHL_N:
    case F_SHR_N:
    case F_ASR_N: /* 5 LSBs of N used */
        sb_fmt(s, "%s %s %u", ca_reg_name[rd], f_op_sym[f], (unsigned)(n & 31));
        break;
    case F_ROR:
    case F_ROL: /* not documented with
                   an immediate */
        sb_fmt(s, "%s %s", ca_reg_name[rd], f_op_sym[f]);
        break;
    default: /* + - # &~ ^ | & */
        sb_fmt(s, "%s %s ", ca_reg_name[rd], f_op_sym[f]);
        sb_imm(s, n);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* CA moves (formats 7a-7d)                                            */

static void dis_ca_move(uint32_t w, dsp3210_insn *ins, sb *s) {
    unsigned direct = !bits(w, 31, 31); /* format 7a: memory-direct */
    unsigned io = bits(w, 25, 25); /* format 7d: memory <-> ioreg */
    unsigned t = bits(w, 24, 24); /* 0 = load reg, 1 = store reg */
    unsigned wsz = bits(w, 23, 21);
    unsigned rh = bits(w, 20, 16);
    const char *kw = w_keyword(wsz);
    char tmp[16];

    if (direct) {
        /* 7a:  rH = (w) *L   /   *L = (w) rH      (L: 16-bit on-chip) */
        unsigned l = bits(w, 15, 0);
        if (bits(w, 25, 25)) {
            ins->status = DSP3210_RESERVED;
            sb_fmt(s, ".word 0x%08x ; reserved (format 7a, bit 25 set)", w);
            return;
        }
        if (t == 0)
            sb_fmt(s, "%s = %s*0x%x", ca_reg_name[rh], kw, l);
        else
            sb_fmt(s, "*0x%x = %s%s", l, kw, ca_reg_name[rh]);
        return;
    }

    if (!io && bits(w, 10, 10)) {
        /* 7b:  rH = (w) iorH  /  iorH = (w) rH */
        unsigned ior = bits(w, 4, 0);
        const char *in = ior_name(ior, tmp, sizeof tmp);

        /* spc pseudo-instructions: stores of r0 to spc (ior10) */
        if (t == 1 && rh == RC_R0 && ior == 10) {
            if (wsz == 7) {
                sb_puts(s, "waiti");
                return;
            }
            if (wsz == 3) {
                sb_puts(s, "bkpt");
                return;
            }
            if (wsz == 0) {
                sb_puts(s, "sftrst");
                return;
            }
        }
        if (t == 0)
            sb_fmt(s, "%s = %s%s", ca_reg_name[rh], kw, in);
        else
            sb_fmt(s, "%s = %s%s", in, kw, ca_reg_name[rh]);
        return;
    }

    /* 7c: rH <-> MEM   |   7d: iorH <-> MEM */
    {
        unsigned rp = bits(w, 15, 11);
        unsigned ri = bits(w, 4, 0);
        const char *regn = io ? ior_name(rh, tmp, sizeof tmp) : ca_reg_name[rh];

        if (t == 0) {
            sb_fmt(s, "%s = %s", regn, kw);
            ca_mem(s, rp, ri);
        } else {
            ca_mem(s, rp, ri);
            sb_fmt(s, " = %s%s", kw, regn);
        }
    }
}

/* ------------------------------------------------------------------ */
/* main decoder                                                        */

static int opcode_is_illegal(unsigned op6) {
    switch (op6) {
    case 0x00:
    case 0x01:
    case 0x02: /* reserved formats 0a/1a/2a */
    case 0x0F: /* DA format 1, M = 111 */
    case 0x16:
    case 0x17: /* DA format 2, M = 11x */
    case 0x22: /* reserved format 2b */
        return 1;
    default:
        return 0;
    }
}

int dsp3210_disassemble(uint32_t w, uint32_t addr, dsp3210_insn *ins) {
    sb s;
    unsigned op6 = w >> 26;
    char tmp[16];

    memset(ins, 0, sizeof *ins);
    ins->word = w;
    ins->addr = addr;
    sb_init(&s, ins->text, sizeof ins->text);

    if (opcode_is_illegal(op6)) {
        ins->status = DSP3210_ILLEGAL;
        sb_fmt(&s, ".word 0x%08x ; illegal opcode 0x%02x", w, op6);
        return ins->status;
    }

    /* DA class: top three bits 001/010/011 (illegal M values filtered) */
    if ((w >> 29) >= 1 && (w >> 29) <= 3) {
        dis_da(w, ins, &s);
        return ins->status;
    }

    switch (op6) {
    case 0x03: { /* 3a: if (rM-- >= 0) goto */
        unsigned rm = bits(w, 25, 21), rb = bits(w, 20, 16);
        ins->is_branch = 1;
        sb_fmt(&s, "if (%s-- >= 0) goto ", ca_reg_name[rm]);
        ca_target(&s, ins, rb, sext16(w));
        break;
    }
    case 0x04: { /* 4a: call {N,rB,rB+N} (rM) */
        unsigned rm = bits(w, 25, 21), rb = bits(w, 20, 16);
        ins->is_branch = 1;
        sb_puts(&s, "call ");
        ca_target(&s, ins, rb, sext16(w));
        sb_fmt(&s, " (%s)", ca_reg_name[rm]);
        break;
    }
    case 0x05:
    case 0x25: { /* 5a/5b: rD = rS3 + N */
        unsigned rd = bits(w, 25, 21), rs3 = bits(w, 20, 16);
        const char *size = (op6 & 0x20) ? "" : "(short) ";
        int32_t n = sext16(w);
        sb_fmt(&s, "%s = %s", ca_reg_name[rd], size);
        if (rs3 == RC_R0) { /* SET: rD = (short) N */
            sb_imm(&s, n);
        } else if (n == 0) {
            sb_puts(&s, ca_reg_name[rs3]);
        } else {
            sb_fmt(&s, "%s %c ", ca_reg_name[rs3], n < 0 ? '-' : '+');
            sb_fmt(&s, "0x%x", (unsigned)(n < 0 ? -n : n));
        }
        break;
    }
    case 0x06:
    case 0x26: /* 6a-6d: ALU */
        if (bits(w, 25, 25))
            dis_ca_alu_imm(w, ins, &s);
        else
            dis_ca_alu_reg(w, ins, &s);
        break;
    case 0x07:
    case 0x27: /* 7a-7d: moves */
        dis_ca_move(w, ins, &s);
        break;
    case 0x20:
    case 0x21: { /* 0b/1b: if (COND) goto */
        unsigned c = bits(w, 26, 21), rb = bits(w, 20, 16);
        int32_t n = sext16(w);
        if (c == 0 && rb == RC_R0 && n == 0) {
            sb_puts(&s, "nop");
            break;
        }
        ins->is_branch = 1;
        if (c == 1 && rb == RC_PCSH && n == 0) {
            /* ireturn: a control transfer with NO delay slot — the word
             * after it is never executed [IM IRETURN "Latency"] */
            ins->no_delay_slot = 1;
            sb_puts(&s, "ireturn");
            break;
        }
        if (c == 1)
            sb_puts(&s, "goto ");
        else
            sb_fmt(&s, "if (%s) goto ", cond_name(c, tmp, sizeof tmp));
        ca_target(&s, ins, rb, n);
        break;
    }
    case 0x23: { /* 3b/3c: do / dolock / doblock */
        unsigned isreg = bits(w, 25, 25);
        unsigned b = bits(w, 24, 24), m = bits(w, 23, 23);
        unsigned k = bits(w, 17, 11);
        const char *mn = m ? (b ? "?do" : "doblock") : (b ? "dolock" : "do");
        if (b && m)
            ins->status = DSP3210_RESERVED;
        sb_puts(&s, mn);
        sb_puts(&s, " ");
        if (!m || k) /* doblock has an implicit K = 0 */
            sb_fmt(&s, "%u, ", k);
        if (isreg)
            sb_puts(&s, ca_reg_name[bits(w, 4, 0)]);
        else
            sb_fmt(&s, "%u", bits(w, 10, 0));
        break;
    }
    case 0x24: { /* 4b: rD = rS <<| N */
        unsigned rd = bits(w, 25, 21), rs = bits(w, 20, 16);
        sb_fmt(&s, "%s = %s <<| 0x%x", ca_reg_name[rd], ca_reg_name[rs], (unsigned)bits(w, 15, 0));
        break;
    }
    case 0x28:
    case 0x29:
    case 0x2A:
    case 0x2B:
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F: { /* 8a: goto {M,rB+M} */
        unsigned rb = bits(w, 20, 16);
        uint32_t m = ((bits(w, 28, 21)) << 16) | bits(w, 15, 0);
        ins->is_branch = 1;
        sb_puts(&s, "goto ");
        if (rb == RC_R0) {
            sb_fmt(&s, "0x%x", m);
            ins->has_target = 1;
            ins->target = m;
        } else if (rb == RC_PC) {
            sb_fmt(&s, "pc+0x%x", m);
            ins->has_target = 1;
            ins->target = ins->addr + 8 + m;
        } else {
            sb_fmt(&s, "%s+0x%x", ca_reg_name[rb], m);
        }
        break;
    }
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x34:
    case 0x35:
    case 0x36:
    case 0x37: { /* 8b: rD=(ushort24)M */
        unsigned rd = bits(w, 20, 16);
        uint32_t m = ((bits(w, 28, 21)) << 16) | bits(w, 15, 0);
        sb_fmt(&s, "%s = (ushort24) 0x%x", ca_reg_name[rd], m);
        break;
    }
    case 0x38:
    case 0x39:
    case 0x3A:
    case 0x3B:
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F: { /* 8c: call M (rM) */
        unsigned rm = bits(w, 20, 16);
        uint32_t m = ((bits(w, 28, 21)) << 16) | bits(w, 15, 0);
        ins->is_branch = 1;
        ins->has_target = 1;
        ins->target = m;
        sb_fmt(&s, "call 0x%x (%s)", m, ca_reg_name[rm]);
        break;
    }
    default:
        /* unreachable: every legal 6-bit opcode is handled above */
        ins->status = DSP3210_RESERVED;
        sb_fmt(&s, ".word 0x%08x", w);
        break;
    }
    return ins->status;
}

/* ------------------------------------------------------------------ */

uint32_t dsp3210_read_be32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

uint32_t dsp3210_read_le32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[0];
}

const char *dsp3210_status_name(int status) {
    switch (status) {
    case DSP3210_OK:
        return "ok";
    case DSP3210_ILLEGAL:
        return "illegal";
    case DSP3210_RESERVED:
        return "reserved";
    default:
        return "?";
    }
}
