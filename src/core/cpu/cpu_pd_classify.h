// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_pd_classify.h
// The 68K decode tree in its third role: the predecode CLASSIFIER
// (proposal §4.3).  Included once per core file, AFTER that core's
// executors, with every OP_ name redefined to return an id instead of
// executing: the generated defaults return the leaf's T1 id, and the
// overrides below return a specialized (T0) id for the operand shapes they
// pre-extract — leaving every other combination of the same leaf at T1.
// Because the tree is shared with the executor and the disassembler, an
// instruction the tree calls illegal is illegal in the cache too.
//
// Template contract: this file is a template (no include guard) and
// expects the includer's cpu_t bindings and CPU_DECODER_IS_68030 setting.
// It defines
//     static uint16_t PD_CLASSIFY_NAME(cpu_t *cpu, const uint8_t *host, uint32_t idx,
//                                      uint32_t avail, uint32_t page_lo,
//                                      pd_entry_t *e, uint32_t *len);
// which fills *e (T1: c = opcode, a:b = extension word; T0: per-id fields)
// and *len (words, T0 only) and returns the id.

#include "cpu_pd_ids.h"

#include <stddef.h>

// Classification context threaded through the tree's OP_ overrides.
typedef struct pd_cls {
    const uint8_t *host; // the page's host bytes
    uint32_t idx; // entry index (word) of the opcode
    uint32_t avail; // words readable from idx to the end of the page
    uint32_t page_lo; // guest address of the page (PC-relative resolution)
    pd_entry_t *e; // out: entry fields
    uint32_t *len; // out: length in words (T0 ids only)
} pd_cls_t;

// Register byte offsets stored in entries (the a/b fields).
#define PD_DOFF(n) ((uint8_t)(offsetof(cpu_t, d) + 4u * (uint32_t)(n)))
#define PD_AOFF(n) ((uint8_t)(offsetof(cpu_t, a) + 4u * (uint32_t)(n)))
_Static_assert(offsetof(cpu_t, a) == offsetof(cpu_t, d) + 32, "d[8]/a[8] must be contiguous");
_Static_assert(offsetof(cpu_t, a) + 32 < 256, "register offsets must fit the entry's byte fields");

// Word n of the instruction (0 = opcode); the caller has checked avail.
static inline uint16_t pd_cw(const pd_cls_t *c, uint32_t n) {
    return LOAD_BE16(c->host + ((c->idx + n) << 1));
}
static inline uint32_t pd_cl(const pd_cls_t *c, uint32_t n) {
    return ((uint32_t)pd_cw(c, n) << 16) | pd_cw(c, n + 1);
}

// A resolved operand shape.
typedef struct pd_shape {
    int sh; // PD_SH_*
    uint8_t off; // register byte offset (D/IND/INC/DEC/D16)
    uint32_t c; // d16 (sign-extended) / absolute address / immediate
    uint32_t words; // extension words consumed
} pd_shape_t;

// Which EA modes a shape resolver may accept.
enum {
    PD_ALLOW_AN = 1, // mode 1 (An as a value)
    PD_ALLOW_IMM = 2, // #imm
    PD_ALLOW_PCREL = 4, // (d16,PC)
    PD_ALLOW_ABS = 8, // (xxx).W / (xxx).L
    PD_ALLOW_MEM = 16, // (An) (An)+ -(An) (d16,An)
    PD_ALLOW_DN = 32, // mode 0
};

// Resolve an operand at `pre` extension words past the opcode.  size is
// the operand size in bytes (immediate width, (An)+ step).  Returns false
// when the shape is not one the executor specializes (T1 territory) or
// its words are not in the page (PD_CROSS decided by the caller).
static inline bool pd_resolve(const pd_cls_t *c, uint32_t mode, uint32_t reg, uint32_t size, uint32_t pre,
                              uint32_t allow, pd_shape_t *s) {
    s->off = 0;
    s->c = 0;
    s->words = 0;
    switch (mode) {
    case 0:
        if (!(allow & PD_ALLOW_DN))
            return false;
        s->sh = PD_SH_D;
        s->off = PD_DOFF(reg);
        return true;
    case 1:
        if (!(allow & PD_ALLOW_AN))
            return false;
        s->sh = PD_SH_D;
        s->off = PD_AOFF(reg);
        return true;
    case 2:
        if (!(allow & PD_ALLOW_MEM))
            return false;
        s->sh = PD_SH_IND;
        s->off = PD_AOFF(reg);
        return true;
    case 3:
    case 4:
        // The A7 byte step (2) is not baked into the handlers: T1.
        if (!(allow & PD_ALLOW_MEM) || (size == 1 && reg == 7))
            return false;
        s->sh = mode == 3 ? PD_SH_INC : PD_SH_DEC;
        s->off = PD_AOFF(reg);
        return true;
    case 5:
        if (!(allow & PD_ALLOW_MEM) || c->avail < pre + 2)
            return false;
        s->sh = PD_SH_D16;
        s->off = PD_AOFF(reg);
        s->c = (uint32_t)(int32_t)(int16_t)pd_cw(c, pre + 1);
        s->words = 1;
        return true;
    case 7:
        switch (reg) {
        case 0: // (xxx).W
            if (!(allow & PD_ALLOW_ABS) || c->avail < pre + 2)
                return false;
            s->sh = PD_SH_ABS;
            s->c = (uint32_t)(int32_t)(int16_t)pd_cw(c, pre + 1);
            s->words = 1;
            return true;
        case 1: // (xxx).L
            if (!(allow & PD_ALLOW_ABS) || c->avail < pre + 3)
                return false;
            s->sh = PD_SH_ABS;
            s->c = pd_cl(c, pre + 1);
            s->words = 2;
            return true;
        case 2: // (d16,PC): the base is the extension word's own address
            if (!(allow & PD_ALLOW_PCREL) || c->avail < pre + 2)
                return false;
            s->sh = PD_SH_ABS;
            s->c = (c->page_lo + ((c->idx + pre + 1) << 1)) + (uint32_t)(int32_t)(int16_t)pd_cw(c, pre + 1);
            s->words = 1;
            return true;
        case 4: // #imm
            if (!(allow & PD_ALLOW_IMM))
                return false;
            if (size == 4) {
                if (c->avail < pre + 3)
                    return false;
                s->c = pd_cl(c, pre + 1);
                s->words = 2;
            } else {
                if (c->avail < pre + 2)
                    return false;
                s->c = size == 1 ? (pd_cw(c, pre + 1) & 0xFFu) : pd_cw(c, pre + 1);
                s->words = 1;
            }
            s->sh = PD_SH_IMM;
            return true;
        default:
            return false;
        }
    default:
        return false; // (d8,An,Xn), full format, (d8,PC,Xn): T1
    }
}

// Commit a T0 decision: the fields and the length.
static inline uint16_t pd_emit(const pd_cls_t *c, uint16_t id, uint8_t a, uint8_t b, uint32_t cval, uint32_t len) {
    c->e->a = a;
    c->e->b = b;
    c->e->c = cval;
    *c->len = len;
    return id;
}

#define PD_SIZE_BYTES(bits) ((bits) / 8u)
#define PD_OP_MODE(op)      (((op) >> 3) & 7u)
#define PD_OP_REG(op)       ((op) & 7u)
#define PD_OP_DN9(op)       (((op) >> 9) & 7u)

// --- ea,Dn families (S7P / S7S): src shape in b (or the length for ABS/IMM), Dn in a ---

// allow_an_w: An sources are legal only at word/long size (never for bytes).
static inline uint16_t pd_cls_ea_dn(const pd_cls_t *c, uint16_t opcode, uint32_t bits, uint16_t fam_base, bool pair,
                                    uint32_t allow, uint16_t t1) {
    pd_shape_t s;
    if (bits == 8)
        allow &= ~(uint32_t)PD_ALLOW_AN;
    if (!pd_resolve(c, PD_OP_MODE(opcode), PD_OP_REG(opcode), PD_SIZE_BYTES(bits), 0, allow, &s))
        return t1;
    uint32_t len = 1 + s.words;
    uint16_t id = (uint16_t)(fam_base + (pair ? 2 : 1) * (uint16_t)s.sh);
    uint8_t b = (s.sh == PD_SH_ABS || s.sh == PD_SH_IMM) ? (uint8_t)len : s.off;
    return pd_emit(c, id, PD_DOFF(PD_OP_DN9(opcode)), b, s.c, len);
}

// Same, but the destination is An (MOVEA/ADDA/SUBA/CMPA).
static inline uint16_t pd_cls_ea_an(const pd_cls_t *c, uint16_t opcode, uint32_t bits, uint16_t fam_base, bool pair,
                                    uint16_t t1) {
    pd_shape_t s;
    uint32_t allow = PD_ALLOW_DN | PD_ALLOW_AN | PD_ALLOW_MEM | PD_ALLOW_ABS | PD_ALLOW_PCREL | PD_ALLOW_IMM;
    if (!pd_resolve(c, PD_OP_MODE(opcode), PD_OP_REG(opcode), PD_SIZE_BYTES(bits), 0, allow, &s))
        return t1;
    uint32_t len = 1 + s.words;
    uint16_t id = (uint16_t)(fam_base + (pair ? 2 : 1) * (uint16_t)s.sh);
    uint8_t b = (s.sh == PD_SH_ABS || s.sh == PD_SH_IMM) ? (uint8_t)len : s.off;
    return pd_emit(c, id, PD_AOFF(PD_OP_DN9(opcode)), b, s.c, len);
}

// --- Dn,ea and #imm,ea families (D6P): dst shape in a (or the length for ABS) ---

static inline uint16_t pd_cls_dn_ea(const pd_cls_t *c, uint16_t opcode, uint32_t bits, uint16_t fam_base,
                                    uint32_t allow, uint16_t t1) {
    pd_shape_t d;
    if (!pd_resolve(c, PD_OP_MODE(opcode), PD_OP_REG(opcode), PD_SIZE_BYTES(bits), 0, allow, &d))
        return t1;
    uint32_t len = 1 + d.words;
    uint8_t a = d.sh == PD_SH_ABS ? (uint8_t)len : d.off;
    return pd_emit(c, (uint16_t)(fam_base + 2 * (uint16_t)d.sh), a, PD_DOFF(PD_OP_DN9(opcode)), d.c, len);
}

// #imm,ea: the immediate comes first, then the destination's extension
// words.  c carries the immediate, or (d16 << 16 | imm16) for the
// byte/word (d16,An) form; a long immediate leaves no room for a
// displacement or an absolute address, so those stay T1.
static inline uint16_t pd_cls_imm_ea(const pd_cls_t *c, uint16_t opcode, uint32_t bits, uint16_t fam_base,
                                     uint32_t allow, uint16_t t1) {
    uint32_t iw = bits == 32 ? 2u : 1u;
    if (c->avail < 1 + iw)
        return t1;
    uint32_t imm = bits == 32 ? pd_cl(c, 1) : bits == 16 ? pd_cw(c, 1) : (pd_cw(c, 1) & 0xFFu);
    pd_shape_t d;
    if (!pd_resolve(c, PD_OP_MODE(opcode), PD_OP_REG(opcode), PD_SIZE_BYTES(bits), iw, allow & ~(uint32_t)PD_ALLOW_ABS,
                    &d))
        return t1;
    uint32_t cval = imm;
    if (d.sh == PD_SH_D16) {
        if (bits == 32)
            return t1; // no room for imm32 and d16
        cval = ((d.c & 0xFFFFu) << 16) | (imm & 0xFFFFu);
    }
    uint32_t len = 1 + iw + d.words;
    return pd_emit(c, (uint16_t)(fam_base + 2 * (uint16_t)d.sh), d.off, (uint8_t)len, cval, len);
}

// ADDQ/SUBQ #q,ea (Dn and memory shapes; the An forms are separate ids).
static inline uint16_t pd_cls_quick(const pd_cls_t *c, uint16_t opcode, uint32_t bits, uint16_t fam_base, uint16_t t1) {
    pd_shape_t d;
    uint32_t allow = PD_ALLOW_DN | PD_ALLOW_MEM | PD_ALLOW_ABS;
    if (!pd_resolve(c, PD_OP_MODE(opcode), PD_OP_REG(opcode), PD_SIZE_BYTES(bits), 0, allow, &d))
        return t1;
    uint32_t q = ((PD_OP_DN9(opcode) - 1u) & 7u) + 1u;
    uint32_t len = 1 + d.words;
    uint8_t a = d.sh == PD_SH_ABS ? (uint8_t)len : d.off;
    return pd_emit(c, (uint16_t)(fam_base + 2 * (uint16_t)d.sh), a, (uint8_t)q, d.c, len);
}

// CLR ea.
static inline uint16_t pd_cls_clr(const pd_cls_t *c, uint16_t opcode, uint32_t bits, uint16_t fam_base, uint16_t t1) {
    pd_shape_t d;
    uint32_t allow = PD_ALLOW_DN | PD_ALLOW_MEM | PD_ALLOW_ABS;
    if (!pd_resolve(c, PD_OP_MODE(opcode), PD_OP_REG(opcode), PD_SIZE_BYTES(bits), 0, allow, &d))
        return t1;
    uint32_t len = 1 + d.words;
    uint8_t a = d.sh == PD_SH_ABS ? (uint8_t)len : d.off;
    return pd_emit(c, (uint16_t)(fam_base + 2 * (uint16_t)d.sh), a, 0, d.c, len);
}

// --- MOVE.sz src,dst: one family per destination shape ---
// Fields: a = dst register offset (or the length when dst is ABS), b = src
// register offset (or the length when src is ABS/IMM), c = whichever side
// needs it; (d16,An)→(d16,An) and #imm.B/W→(d16,An) pack both halves.
static inline uint16_t pd_cls_move(const pd_cls_t *c, uint16_t opcode, uint32_t bits, uint16_t fam_d, uint16_t t1) {
    pd_shape_t s, d;
    uint32_t allow_s =
        PD_ALLOW_DN | PD_ALLOW_MEM | PD_ALLOW_ABS | PD_ALLOW_PCREL | PD_ALLOW_IMM | (bits == 8 ? 0u : PD_ALLOW_AN);
    if (!pd_resolve(c, PD_OP_MODE(opcode), PD_OP_REG(opcode), PD_SIZE_BYTES(bits), 0, allow_s, &s))
        return t1;
    uint32_t dmode = (opcode >> 6) & 7u, dreg = PD_OP_DN9(opcode);
    if (!pd_resolve(c, dmode, dreg, PD_SIZE_BYTES(bits), s.words, PD_ALLOW_DN | PD_ALLOW_MEM | PD_ALLOW_ABS, &d))
        return t1;
    bool s_needs_c = s.sh == PD_SH_D16 || s.sh == PD_SH_ABS || s.sh == PD_SH_IMM;
    bool d_needs_c = d.sh == PD_SH_D16 || d.sh == PD_SH_ABS;
    uint32_t cval = s_needs_c ? s.c : d.c;
    if (s_needs_c && d_needs_c) {
        // Only the packed forms fit: (d16,An)→(d16,An), #imm.B/W→(d16,An).
        if (d.sh != PD_SH_D16)
            return t1;
        if (s.sh == PD_SH_D16 || (s.sh == PD_SH_IMM && bits != 32))
            cval = ((d.c & 0xFFFFu) << 16) | (s.c & 0xFFFFu);
        else
            return t1;
    }
    uint32_t len = 1 + s.words + d.words;
    bool s_len = s.sh == PD_SH_ABS || s.sh == PD_SH_IMM;
    bool d_len = d.sh == PD_SH_ABS;
    if (s_len && d_len)
        return t1; // both sides would need the length field
    uint8_t a = d_len ? (uint8_t)len : d.off;
    uint8_t b = s_len ? (uint8_t)len : s.off;
    // Family of the destination shape: the six families are consecutive.
    uint16_t fam = (uint16_t)(fam_d + PD_S7P_SLOTS * (uint16_t)d.sh);
    return pd_emit(c, (uint16_t)(fam + 2 * (uint16_t)s.sh), a, b, cval, len);
}

// --- register-only unaries: a = Dn ---
static inline uint16_t pd_cls_dn_only(const pd_cls_t *c, uint16_t opcode, uint16_t id, uint16_t t1) {
    if (PD_OP_MODE(opcode) != 0)
        return t1;
    return pd_emit(c, id, PD_DOFF(PD_OP_REG(opcode)), 0, 0, 1);
}

// --- shifts: a = Dy, b = count (immediate 1..8) or Dx offset ---
static inline uint16_t pd_cls_shift_imm(const pd_cls_t *c, uint16_t opcode, uint16_t id) {
    uint32_t q = ((PD_OP_DN9(opcode) - 1u) & 7u) + 1u;
    return pd_emit(c, id, PD_DOFF(PD_OP_REG(opcode)), (uint8_t)q, 0, 1);
}
static inline uint16_t pd_cls_shift_reg(const pd_cls_t *c, uint16_t opcode, uint16_t id) {
    return pd_emit(c, id, PD_DOFF(PD_OP_REG(opcode)), PD_DOFF(PD_OP_DN9(opcode)), 0, 1);
}

// --- Bcc / BSR / DBcc: in-page targets as entry indices, others as PCs ---
// target = instruction address + 2 + displacement; odd targets and
// targets outside the page take the _OUT id (relookup from the PC).
static inline bool pd_in_page(const pd_cls_t *c, uint32_t target, uint32_t *index) {
    if (target & 1u)
        return false;
    if ((target & ~(uint32_t)PAGE_MASK) != c->page_lo)
        return false;
    *index = (target & PAGE_MASK) >> 1;
    return true;
}

static inline uint16_t pd_cls_bcc(const pd_cls_t *c, uint16_t opcode, bool word, uint16_t t1) {
    uint32_t cond = (opcode >> 8) & 0xFu; // 0 = BRA, 1 = BSR
    uint32_t ipc = c->page_lo + (c->idx << 1);
    int32_t disp;
    uint32_t len;
    if (word) {
        if (c->avail < 2)
            return t1;
        disp = (int32_t)(int16_t)pd_cw(c, 1);
        len = 2;
    } else {
        disp = (int32_t)(int8_t)(opcode & 0xFFu);
        len = 1;
    }
    uint32_t target = ipc + 2 + (uint32_t)disp;
    uint32_t index;
    bool in = pd_in_page(c, target, &index);
    uint16_t id;
    if (cond == 1)
        id = word ? (in ? PDF_BSR_W_IN : PDF_BSR_W_OUT) : (in ? PDF_BSR_B_IN : PDF_BSR_B_OUT);
    else if (word)
        id = (uint16_t)((in ? PDF_BCC_W_IN : PDF_BCC_W_OUT) + cond);
    else
        id = (uint16_t)((in ? PDF_BCC_B_IN : PDF_BCC_B_OUT) + cond);
    return pd_emit(c, id, (uint8_t)len, 0, in ? index : target, len);
}

static inline uint16_t pd_cls_dbcc(const pd_cls_t *c, uint16_t opcode, uint16_t t1) {
    if (c->avail < 2)
        return t1;
    uint32_t cond = (opcode >> 8) & 0xFu;
    uint32_t ipc = c->page_lo + (c->idx << 1);
    uint32_t target = ipc + 2 + (uint32_t)(int32_t)(int16_t)pd_cw(c, 1);
    uint32_t index;
    bool in = pd_in_page(c, target, &index);
    uint16_t id = cond == 1 ? (in ? PDF_DBF_IN : PDF_DBF_OUT) : (in ? PDF_DBCC_IN : PDF_DBCC_OUT);
    return pd_emit(c, id, (uint8_t)cond, PD_DOFF(PD_OP_REG(opcode)), in ? index : target, 2);
}

// --- JMP/JSR/LEA/PEA control EAs: (An), (d16,An), (xxx).L/.W, (d16,PC) ---
static inline uint16_t pd_cls_control(const pd_cls_t *c, uint16_t opcode, uint16_t id_ind, uint16_t id_d16,
                                      uint16_t id_abs, uint8_t a, uint16_t t1) {
    pd_shape_t s;
    if (!pd_resolve(c, PD_OP_MODE(opcode), PD_OP_REG(opcode), 4, 0, PD_ALLOW_MEM | PD_ALLOW_ABS | PD_ALLOW_PCREL, &s))
        return t1;
    uint32_t len = 1 + s.words;
    switch (s.sh) {
    case PD_SH_IND:
        return pd_emit(c, id_ind, a ? a : (uint8_t)len, s.off, 0, len);
    case PD_SH_D16:
        return pd_emit(c, id_d16, a ? a : (uint8_t)len, s.off, s.c, len);
    case PD_SH_ABS:
        return pd_emit(c, id_abs, a ? a : (uint8_t)len, (uint8_t)len, s.c, len);
    default:
        return t1; // (An)+ / -(An) are not control EAs
    }
}

// --- MOVEM: c = mask | d16 << 16, a = An, b = length ---
static inline uint16_t pd_cls_movem(const pd_cls_t *c, uint16_t opcode, bool to_regs, uint16_t fam_w_base,
                                    uint16_t fam_l_base, uint16_t t1) {
    if (c->avail < 2)
        return t1;
    uint32_t mask = pd_cw(c, 1);
    uint32_t mode = PD_OP_MODE(opcode), reg = PD_OP_REG(opcode);
    bool is_long = (opcode & 0x0040u) != 0;
    uint16_t base = is_long ? fam_l_base : fam_w_base;
    uint32_t len = 2, cval = mask;
    uint16_t id;
    if (mode == 2)
        id = (uint16_t)(base + (to_regs ? 4 : 1)); // IND
    else if (mode == 3 && to_regs)
        id = (uint16_t)(base + 3); // (An)+ → regs
    else if (mode == 4 && !to_regs)
        id = (uint16_t)(base + 0); // regs → -(An)
    else if (mode == 5) {
        if (c->avail < 3)
            return t1;
        cval |= (uint32_t)pd_cw(c, 2) << 16;
        len = 3;
        id = (uint16_t)(base + (to_regs ? 5 : 2));
    } else
        return t1;
    return pd_emit(c, id, PD_AOFF(reg), (uint8_t)len, cval, len);
}

// ============================================================================
// The classifier instantiation of the tree.
// ============================================================================

#include "cpu_pd_t1_classify.h" // generated: every OP_ → return T1_<leaf>

// Model-dependent operand rules the executor's shapes must respect.
#ifdef CPU_DECODER_IS_68030
#define PD_TST_ALLOW  (PD_ALLOW_DN | PD_ALLOW_AN | PD_ALLOW_MEM | PD_ALLOW_ABS | PD_ALLOW_PCREL | PD_ALLOW_IMM)
#define PD_CMPI_ALLOW (PD_ALLOW_DN | PD_ALLOW_MEM)
#else
#define PD_TST_ALLOW  (PD_ALLOW_DN | PD_ALLOW_MEM | PD_ALLOW_ABS)
#define PD_CMPI_ALLOW (PD_ALLOW_DN | PD_ALLOW_MEM)
#endif
#define PD_DATA_ALLOW    (PD_ALLOW_DN | PD_ALLOW_MEM | PD_ALLOW_ABS | PD_ALLOW_PCREL | PD_ALLOW_IMM)
#define PD_ANY_ALLOW     (PD_DATA_ALLOW | PD_ALLOW_AN)
#define PD_MEMALT_ALLOW  (PD_ALLOW_MEM | PD_ALLOW_ABS)
#define PD_DATAALT_ALLOW (PD_ALLOW_DN | PD_ALLOW_MEM | PD_ALLOW_ABS)

// clang-format off
// MOVE
#undef OP_MOVE_B_EA_EA
#define OP_MOVE_B_EA_EA return pd_cls_move(ctx, opcode, 8, PDF_MOVE_B_D, T1_MOVE_B_EA_EA)
#undef OP_MOVE_W_EA_EA
#define OP_MOVE_W_EA_EA return pd_cls_move(ctx, opcode, 16, PDF_MOVE_W_D, T1_MOVE_W_EA_EA)
#undef OP_MOVE_L_EA_EA
#define OP_MOVE_L_EA_EA return pd_cls_move(ctx, opcode, 32, PDF_MOVE_L_D, T1_MOVE_L_EA_EA)
#undef OP_MOVEA_W_EA_AN
#define OP_MOVEA_W_EA_AN return pd_cls_ea_an(ctx, opcode, 16, PDF_MOVEA_W, false, T1_MOVEA_W_EA_AN)
#undef OP_MOVEA_L_EA_AN
#define OP_MOVEA_L_EA_AN return pd_cls_ea_an(ctx, opcode, 32, PDF_MOVEA_L, false, T1_MOVEA_L_EA_AN)
#undef OP_MOVEQ_L_DATA_DN
#define OP_MOVEQ_L_DATA_DN return pd_emit(ctx, PDF_MOVEQ, PD_DOFF(PD_OP_DN9(opcode)), 0, (uint32_t)(int32_t)(int8_t)opcode, 1)

// ea,Dn
#undef OP_ADD_B_EA_DN
#define OP_ADD_B_EA_DN return pd_cls_ea_dn(ctx, opcode, 8, PDF_ADD_B_EA_DN, true, PD_ANY_ALLOW, T1_ADD_B_EA_DN)
#undef OP_ADD_W_EA_DN
#define OP_ADD_W_EA_DN return pd_cls_ea_dn(ctx, opcode, 16, PDF_ADD_W_EA_DN, true, PD_ANY_ALLOW, T1_ADD_W_EA_DN)
#undef OP_ADD_L_EA_DN
#define OP_ADD_L_EA_DN return pd_cls_ea_dn(ctx, opcode, 32, PDF_ADD_L_EA_DN, true, PD_ANY_ALLOW, T1_ADD_L_EA_DN)
#undef OP_SUB_B_EA_DN
#define OP_SUB_B_EA_DN return pd_cls_ea_dn(ctx, opcode, 8, PDF_SUB_B_EA_DN, true, PD_ANY_ALLOW, T1_SUB_B_EA_DN)
#undef OP_SUB_W_EA_DN
#define OP_SUB_W_EA_DN return pd_cls_ea_dn(ctx, opcode, 16, PDF_SUB_W_EA_DN, true, PD_ANY_ALLOW, T1_SUB_W_EA_DN)
#undef OP_SUB_L_EA_DN
#define OP_SUB_L_EA_DN return pd_cls_ea_dn(ctx, opcode, 32, PDF_SUB_L_EA_DN, true, PD_ANY_ALLOW, T1_SUB_L_EA_DN)
#undef OP_AND_B_EA_DN
#define OP_AND_B_EA_DN return pd_cls_ea_dn(ctx, opcode, 8, PDF_AND_B_EA_DN, true, PD_DATA_ALLOW, T1_AND_B_EA_DN)
#undef OP_AND_W_EA_DN
#define OP_AND_W_EA_DN return pd_cls_ea_dn(ctx, opcode, 16, PDF_AND_W_EA_DN, true, PD_DATA_ALLOW, T1_AND_W_EA_DN)
#undef OP_AND_L_EA_DN
#define OP_AND_L_EA_DN return pd_cls_ea_dn(ctx, opcode, 32, PDF_AND_L_EA_DN, true, PD_DATA_ALLOW, T1_AND_L_EA_DN)
#undef OP_OR_B_EA_DN
#define OP_OR_B_EA_DN return pd_cls_ea_dn(ctx, opcode, 8, PDF_OR_B_EA_DN, true, PD_DATA_ALLOW, T1_OR_B_EA_DN)
#undef OP_OR_W_EA_DN
#define OP_OR_W_EA_DN return pd_cls_ea_dn(ctx, opcode, 16, PDF_OR_W_EA_DN, true, PD_DATA_ALLOW, T1_OR_W_EA_DN)
#undef OP_OR_L_EA_DN
#define OP_OR_L_EA_DN return pd_cls_ea_dn(ctx, opcode, 32, PDF_OR_L_EA_DN, true, PD_DATA_ALLOW, T1_OR_L_EA_DN)
#undef OP_CMP_B_EA_DN
#define OP_CMP_B_EA_DN return pd_cls_ea_dn(ctx, opcode, 8, PDF_CMP_B_EA_DN, true, PD_ANY_ALLOW, T1_CMP_B_EA_DN)
#undef OP_CMP_W_EA_DN
#define OP_CMP_W_EA_DN return pd_cls_ea_dn(ctx, opcode, 16, PDF_CMP_W_EA_DN, true, PD_ANY_ALLOW, T1_CMP_W_EA_DN)
#undef OP_CMP_L_EA_DN
#define OP_CMP_L_EA_DN return pd_cls_ea_dn(ctx, opcode, 32, PDF_CMP_L_EA_DN, true, PD_ANY_ALLOW, T1_CMP_L_EA_DN)

// Dn,ea
#undef OP_ADD_B_DN_EA
#define OP_ADD_B_DN_EA return pd_cls_dn_ea(ctx, opcode, 8, PDF_ADD_B_DN_EA, PD_MEMALT_ALLOW, T1_ADD_B_DN_EA)
#undef OP_ADD_W_DN_EA
#define OP_ADD_W_DN_EA return pd_cls_dn_ea(ctx, opcode, 16, PDF_ADD_W_DN_EA, PD_MEMALT_ALLOW, T1_ADD_W_DN_EA)
#undef OP_ADD_L_DN_EA
#define OP_ADD_L_DN_EA return pd_cls_dn_ea(ctx, opcode, 32, PDF_ADD_L_DN_EA, PD_MEMALT_ALLOW, T1_ADD_L_DN_EA)
#undef OP_SUB_B_DN_EA
#define OP_SUB_B_DN_EA return pd_cls_dn_ea(ctx, opcode, 8, PDF_SUB_B_DN_EA, PD_MEMALT_ALLOW, T1_SUB_B_DN_EA)
#undef OP_SUB_W_DN_EA
#define OP_SUB_W_DN_EA return pd_cls_dn_ea(ctx, opcode, 16, PDF_SUB_W_DN_EA, PD_MEMALT_ALLOW, T1_SUB_W_DN_EA)
#undef OP_SUB_L_DN_EA
#define OP_SUB_L_DN_EA return pd_cls_dn_ea(ctx, opcode, 32, PDF_SUB_L_DN_EA, PD_MEMALT_ALLOW, T1_SUB_L_DN_EA)
#undef OP_AND_B_DN_EA
#define OP_AND_B_DN_EA return pd_cls_dn_ea(ctx, opcode, 8, PDF_AND_B_DN_EA, PD_MEMALT_ALLOW, T1_AND_B_DN_EA)
#undef OP_AND_W_DN_EA
#define OP_AND_W_DN_EA return pd_cls_dn_ea(ctx, opcode, 16, PDF_AND_W_DN_EA, PD_MEMALT_ALLOW, T1_AND_W_DN_EA)
#undef OP_AND_L_DN_EA
#define OP_AND_L_DN_EA return pd_cls_dn_ea(ctx, opcode, 32, PDF_AND_L_DN_EA, PD_MEMALT_ALLOW, T1_AND_L_DN_EA)
#undef OP_OR_B_DN_EA
#define OP_OR_B_DN_EA return pd_cls_dn_ea(ctx, opcode, 8, PDF_OR_B_DN_EA, PD_MEMALT_ALLOW, T1_OR_B_DN_EA)
#undef OP_OR_W_DN_EA
#define OP_OR_W_DN_EA return pd_cls_dn_ea(ctx, opcode, 16, PDF_OR_W_DN_EA, PD_MEMALT_ALLOW, T1_OR_W_DN_EA)
#undef OP_OR_L_DN_EA
#define OP_OR_L_DN_EA return pd_cls_dn_ea(ctx, opcode, 32, PDF_OR_L_DN_EA, PD_MEMALT_ALLOW, T1_OR_L_DN_EA)
#undef OP_EOR_B_DN_EA
#define OP_EOR_B_DN_EA return pd_cls_dn_ea(ctx, opcode, 8, PDF_EOR_B_DN_EA, PD_DATAALT_ALLOW, T1_EOR_B_DN_EA)
#undef OP_EOR_W_DN_EA
#define OP_EOR_W_DN_EA return pd_cls_dn_ea(ctx, opcode, 16, PDF_EOR_W_DN_EA, PD_DATAALT_ALLOW, T1_EOR_W_DN_EA)
#undef OP_EOR_L_DN_EA
#define OP_EOR_L_DN_EA return pd_cls_dn_ea(ctx, opcode, 32, PDF_EOR_L_DN_EA, PD_DATAALT_ALLOW, T1_EOR_L_DN_EA)

// #imm,ea
#undef OP_ADDI_B_DATA_EA
#define OP_ADDI_B_DATA_EA return pd_cls_imm_ea(ctx, opcode, 8, PDF_ADDI_B, PD_DATAALT_ALLOW, T1_ADDI_B_DATA_EA)
#undef OP_ADDI_W_DATA_EA
#define OP_ADDI_W_DATA_EA return pd_cls_imm_ea(ctx, opcode, 16, PDF_ADDI_W, PD_DATAALT_ALLOW, T1_ADDI_W_DATA_EA)
#undef OP_ADDI_L_DATA_EA
#define OP_ADDI_L_DATA_EA return pd_cls_imm_ea(ctx, opcode, 32, PDF_ADDI_L, PD_DATAALT_ALLOW, T1_ADDI_L_DATA_EA)
#undef OP_SUBI_B_DATA_EA
#define OP_SUBI_B_DATA_EA return pd_cls_imm_ea(ctx, opcode, 8, PDF_SUBI_B, PD_DATAALT_ALLOW, T1_SUBI_B_DATA_EA)
#undef OP_SUBI_W_DATA_EA
#define OP_SUBI_W_DATA_EA return pd_cls_imm_ea(ctx, opcode, 16, PDF_SUBI_W, PD_DATAALT_ALLOW, T1_SUBI_W_DATA_EA)
#undef OP_SUBI_L_DATA_EA
#define OP_SUBI_L_DATA_EA return pd_cls_imm_ea(ctx, opcode, 32, PDF_SUBI_L, PD_DATAALT_ALLOW, T1_SUBI_L_DATA_EA)
#undef OP_ANDI_B_DATA_EA
#define OP_ANDI_B_DATA_EA return pd_cls_imm_ea(ctx, opcode, 8, PDF_ANDI_B, PD_DATAALT_ALLOW, T1_ANDI_B_DATA_EA)
#undef OP_ANDI_W_DATA_EA
#define OP_ANDI_W_DATA_EA return pd_cls_imm_ea(ctx, opcode, 16, PDF_ANDI_W, PD_DATAALT_ALLOW, T1_ANDI_W_DATA_EA)
#undef OP_ANDI_L_DATA_EA
#define OP_ANDI_L_DATA_EA return pd_cls_imm_ea(ctx, opcode, 32, PDF_ANDI_L, PD_DATAALT_ALLOW, T1_ANDI_L_DATA_EA)
#undef OP_ORI_B_DATA_EA
#define OP_ORI_B_DATA_EA return pd_cls_imm_ea(ctx, opcode, 8, PDF_ORI_B, PD_DATAALT_ALLOW, T1_ORI_B_DATA_EA)
#undef OP_ORI_W_DATA_EA
#define OP_ORI_W_DATA_EA return pd_cls_imm_ea(ctx, opcode, 16, PDF_ORI_W, PD_DATAALT_ALLOW, T1_ORI_W_DATA_EA)
#undef OP_ORI_L_DATA_EA
#define OP_ORI_L_DATA_EA return pd_cls_imm_ea(ctx, opcode, 32, PDF_ORI_L, PD_DATAALT_ALLOW, T1_ORI_L_DATA_EA)
#undef OP_EORI_B_DATA_EA
#define OP_EORI_B_DATA_EA return pd_cls_imm_ea(ctx, opcode, 8, PDF_EORI_B, PD_DATAALT_ALLOW, T1_EORI_B_DATA_EA)
#undef OP_EORI_W_DATA_EA
#define OP_EORI_W_DATA_EA return pd_cls_imm_ea(ctx, opcode, 16, PDF_EORI_W, PD_DATAALT_ALLOW, T1_EORI_W_DATA_EA)
#undef OP_EORI_L_DATA_EA
#define OP_EORI_L_DATA_EA return pd_cls_imm_ea(ctx, opcode, 32, PDF_EORI_L, PD_DATAALT_ALLOW, T1_EORI_L_DATA_EA)
#undef OP_CMPI_B_DATA_EA
#define OP_CMPI_B_DATA_EA return pd_cls_imm_ea(ctx, opcode, 8, PDF_CMPI_B, PD_CMPI_ALLOW, T1_CMPI_B_DATA_EA)
#undef OP_CMPI_W_DATA_EA
#define OP_CMPI_W_DATA_EA return pd_cls_imm_ea(ctx, opcode, 16, PDF_CMPI_W, PD_CMPI_ALLOW, T1_CMPI_W_DATA_EA)
#undef OP_CMPI_L_DATA_EA
#define OP_CMPI_L_DATA_EA return pd_cls_imm_ea(ctx, opcode, 32, PDF_CMPI_L, PD_CMPI_ALLOW, T1_CMPI_L_DATA_EA)

// ADDQ/SUBQ
#undef OP_ADDQ_B_DATA_EA
#define OP_ADDQ_B_DATA_EA return pd_cls_quick(ctx, opcode, 8, PDF_ADDQ_B, T1_ADDQ_B_DATA_EA)
#undef OP_ADDQ_W_DATA_EA
#define OP_ADDQ_W_DATA_EA return pd_cls_quick(ctx, opcode, 16, PDF_ADDQ_W, T1_ADDQ_W_DATA_EA)
#undef OP_ADDQ_L_DATA_EA
#define OP_ADDQ_L_DATA_EA return pd_cls_quick(ctx, opcode, 32, PDF_ADDQ_L, T1_ADDQ_L_DATA_EA)
#undef OP_SUBQ_B_DATA_EA
#define OP_SUBQ_B_DATA_EA return pd_cls_quick(ctx, opcode, 8, PDF_SUBQ_B, T1_SUBQ_B_DATA_EA)
#undef OP_SUBQ_W_DATA_EA
#define OP_SUBQ_W_DATA_EA return pd_cls_quick(ctx, opcode, 16, PDF_SUBQ_W, T1_SUBQ_W_DATA_EA)
#undef OP_SUBQ_L_DATA_EA
#define OP_SUBQ_L_DATA_EA return pd_cls_quick(ctx, opcode, 32, PDF_SUBQ_L, T1_SUBQ_L_DATA_EA)
#undef OP_ADDQ_W_DATA_AN
#define OP_ADDQ_W_DATA_AN return pd_emit(ctx, PDF_ADDQ_AN, PD_AOFF(PD_OP_REG(opcode)), (uint8_t)(((PD_OP_DN9(opcode) - 1u) & 7u) + 1u), 0, 1)
#undef OP_ADDQ_L_DATA_AN
#define OP_ADDQ_L_DATA_AN return pd_emit(ctx, PDF_ADDQ_AN, PD_AOFF(PD_OP_REG(opcode)), (uint8_t)(((PD_OP_DN9(opcode) - 1u) & 7u) + 1u), 0, 1)
#undef OP_SUBQ_W_DATA_AN
#define OP_SUBQ_W_DATA_AN return pd_emit(ctx, PDF_SUBQ_AN, PD_AOFF(PD_OP_REG(opcode)), (uint8_t)(((PD_OP_DN9(opcode) - 1u) & 7u) + 1u), 0, 1)
#undef OP_SUBQ_L_DATA_AN
#define OP_SUBQ_L_DATA_AN return pd_emit(ctx, PDF_SUBQ_AN, PD_AOFF(PD_OP_REG(opcode)), (uint8_t)(((PD_OP_DN9(opcode) - 1u) & 7u) + 1u), 0, 1)

// ADDA/SUBA/CMPA
#undef OP_ADDA_W_EA_AN
#define OP_ADDA_W_EA_AN return pd_cls_ea_an(ctx, opcode, 16, PDF_ADDA_W, false, T1_ADDA_W_EA_AN)
#undef OP_ADDA_L_EA_AN
#define OP_ADDA_L_EA_AN return pd_cls_ea_an(ctx, opcode, 32, PDF_ADDA_L, false, T1_ADDA_L_EA_AN)
#undef OP_SUBA_W_EA_AN
#define OP_SUBA_W_EA_AN return pd_cls_ea_an(ctx, opcode, 16, PDF_SUBA_W, false, T1_SUBA_W_EA_AN)
#undef OP_SUBA_L_EA_AN
#define OP_SUBA_L_EA_AN return pd_cls_ea_an(ctx, opcode, 32, PDF_SUBA_L, false, T1_SUBA_L_EA_AN)
#undef OP_CMPA_W_EA_AN
#define OP_CMPA_W_EA_AN return pd_cls_ea_an(ctx, opcode, 16, PDF_CMPA_W, true, T1_CMPA_W_EA_AN)
#undef OP_CMPA_L_EA_AN
#define OP_CMPA_L_EA_AN return pd_cls_ea_an(ctx, opcode, 32, PDF_CMPA_L, true, T1_CMPA_L_EA_AN)

// TST / CLR
#undef OP_TST_B_EA
#define OP_TST_B_EA return pd_cls_ea_dn(ctx, opcode, 8, PDF_TST_B, true, PD_TST_ALLOW, T1_TST_B_EA)
#undef OP_TST_W_EA
#define OP_TST_W_EA return pd_cls_ea_dn(ctx, opcode, 16, PDF_TST_W, true, PD_TST_ALLOW, T1_TST_W_EA)
#undef OP_TST_L_EA
#define OP_TST_L_EA return pd_cls_ea_dn(ctx, opcode, 32, PDF_TST_L, true, PD_TST_ALLOW, T1_TST_L_EA)
#undef OP_CLR_B_EA
#define OP_CLR_B_EA return pd_cls_clr(ctx, opcode, 8, PDF_CLR_B, T1_CLR_B_EA)
#undef OP_CLR_W_EA
#define OP_CLR_W_EA return pd_cls_clr(ctx, opcode, 16, PDF_CLR_W, T1_CLR_W_EA)
#undef OP_CLR_L_EA
#define OP_CLR_L_EA return pd_cls_clr(ctx, opcode, 32, PDF_CLR_L, T1_CLR_L_EA)

// register-only unaries
#undef OP_NEG_B_EA
#define OP_NEG_B_EA return pd_cls_dn_only(ctx, opcode, PDF_NEG_B_D, T1_NEG_B_EA)
#undef OP_NEG_W_EA
#define OP_NEG_W_EA return pd_cls_dn_only(ctx, opcode, PDF_NEG_W_D, T1_NEG_W_EA)
#undef OP_NEG_L_EA
#define OP_NEG_L_EA return pd_cls_dn_only(ctx, opcode, PDF_NEG_L_D, T1_NEG_L_EA)
#undef OP_NOT_B_EA
#define OP_NOT_B_EA return pd_cls_dn_only(ctx, opcode, PDF_NOT_B_D, T1_NOT_B_EA)
#undef OP_NOT_W_EA
#define OP_NOT_W_EA return pd_cls_dn_only(ctx, opcode, PDF_NOT_W_D, T1_NOT_W_EA)
#undef OP_NOT_L_EA
#define OP_NOT_L_EA return pd_cls_dn_only(ctx, opcode, PDF_NOT_L_D, T1_NOT_L_EA)
#undef OP_EXT_W_DN
#define OP_EXT_W_DN return pd_cls_dn_only(ctx, opcode, PDF_EXT_W, T1_EXT_W_DN)
#undef OP_EXT_L_DN
#define OP_EXT_L_DN return pd_cls_dn_only(ctx, opcode, PDF_EXT_L, T1_EXT_L_DN)
#undef OP_SWAP_DN
#define OP_SWAP_DN return pd_cls_dn_only(ctx, opcode, PDF_SWAP, T1_SWAP_DN)

// shifts: immediate count forms (DATA) and register count forms (DX)
#define PD_SHIFT_I(name, fam) \
    return pd_cls_shift_imm(ctx, opcode, PDF_##fam)
#define PD_SHIFT_R(name, fam) \
    return pd_cls_shift_reg(ctx, opcode, PDF_##fam)
#undef OP_ASL_B_DATA_DY
#define OP_ASL_B_DATA_DY PD_SHIFT_I(ASL_B_DATA_DY, ASL_B_I)
#undef OP_ASL_W_DATA_DY
#define OP_ASL_W_DATA_DY PD_SHIFT_I(ASL_W_DATA_DY, ASL_W_I)
#undef OP_ASL_L_DATA_DY
#define OP_ASL_L_DATA_DY PD_SHIFT_I(ASL_L_DATA_DY, ASL_L_I)
#undef OP_ASR_B_DATA_DY
#define OP_ASR_B_DATA_DY PD_SHIFT_I(ASR_B_DATA_DY, ASR_B_I)
#undef OP_ASR_W_DATA_DY
#define OP_ASR_W_DATA_DY PD_SHIFT_I(ASR_W_DATA_DY, ASR_W_I)
#undef OP_ASR_L_DATA_DY
#define OP_ASR_L_DATA_DY PD_SHIFT_I(ASR_L_DATA_DY, ASR_L_I)
#undef OP_LSL_B_DATA_DY
#define OP_LSL_B_DATA_DY PD_SHIFT_I(LSL_B_DATA_DY, LSL_B_I)
#undef OP_LSL_W_DATA_DY
#define OP_LSL_W_DATA_DY PD_SHIFT_I(LSL_W_DATA_DY, LSL_W_I)
#undef OP_LSL_L_DATA_DY
#define OP_LSL_L_DATA_DY PD_SHIFT_I(LSL_L_DATA_DY, LSL_L_I)
#undef OP_LSR_B_DATA_DY
#define OP_LSR_B_DATA_DY PD_SHIFT_I(LSR_B_DATA_DY, LSR_B_I)
#undef OP_LSR_W_DATA_DY
#define OP_LSR_W_DATA_DY PD_SHIFT_I(LSR_W_DATA_DY, LSR_W_I)
#undef OP_LSR_L_DATA_DY
#define OP_LSR_L_DATA_DY PD_SHIFT_I(LSR_L_DATA_DY, LSR_L_I)
#undef OP_ROL_B_DATA_DY
#define OP_ROL_B_DATA_DY PD_SHIFT_I(ROL_B_DATA_DY, ROL_B_I)
#undef OP_ROL_W_DATA_DY
#define OP_ROL_W_DATA_DY PD_SHIFT_I(ROL_W_DATA_DY, ROL_W_I)
#undef OP_ROL_L_DATA_DY
#define OP_ROL_L_DATA_DY PD_SHIFT_I(ROL_L_DATA_DY, ROL_L_I)
#undef OP_ROR_B_DATA_DY
#define OP_ROR_B_DATA_DY PD_SHIFT_I(ROR_B_DATA_DY, ROR_B_I)
#undef OP_ROR_W_DATA_DY
#define OP_ROR_W_DATA_DY PD_SHIFT_I(ROR_W_DATA_DY, ROR_W_I)
#undef OP_ROR_L_DATA_DY
#define OP_ROR_L_DATA_DY PD_SHIFT_I(ROR_L_DATA_DY, ROR_L_I)
#undef OP_ASL_B_DX_DY
#define OP_ASL_B_DX_DY PD_SHIFT_R(ASL_B_DX_DY, ASL_B_R)
#undef OP_ASL_W_DX_DY
#define OP_ASL_W_DX_DY PD_SHIFT_R(ASL_W_DX_DY, ASL_W_R)
#undef OP_ASL_L_DX_DY
#define OP_ASL_L_DX_DY PD_SHIFT_R(ASL_L_DX_DY, ASL_L_R)
#undef OP_ASR_B_DX_DY
#define OP_ASR_B_DX_DY PD_SHIFT_R(ASR_B_DX_DY, ASR_B_R)
#undef OP_ASR_W_DX_DY
#define OP_ASR_W_DX_DY PD_SHIFT_R(ASR_W_DX_DY, ASR_W_R)
#undef OP_ASR_L_DX_DY
#define OP_ASR_L_DX_DY PD_SHIFT_R(ASR_L_DX_DY, ASR_L_R)
#undef OP_LSL_B_DX_DY
#define OP_LSL_B_DX_DY PD_SHIFT_R(LSL_B_DX_DY, LSL_B_R)
#undef OP_LSL_W_DX_DY
#define OP_LSL_W_DX_DY PD_SHIFT_R(LSL_W_DX_DY, LSL_W_R)
#undef OP_LSL_L_DX_DY
#define OP_LSL_L_DX_DY PD_SHIFT_R(LSL_L_DX_DY, LSL_L_R)
#undef OP_LSR_B_DX_DY
#define OP_LSR_B_DX_DY PD_SHIFT_R(LSR_B_DX_DY, LSR_B_R)
#undef OP_LSR_W_DX_DY
#define OP_LSR_W_DX_DY PD_SHIFT_R(LSR_W_DX_DY, LSR_W_R)
#undef OP_LSR_L_DX_DY
#define OP_LSR_L_DX_DY PD_SHIFT_R(LSR_L_DX_DY, LSR_L_R)
#undef OP_ROL_B_DX_DY
#define OP_ROL_B_DX_DY PD_SHIFT_R(ROL_B_DX_DY, ROL_B_R)
#undef OP_ROL_W_DX_DY
#define OP_ROL_W_DX_DY PD_SHIFT_R(ROL_W_DX_DY, ROL_W_R)
#undef OP_ROL_L_DX_DY
#define OP_ROL_L_DX_DY PD_SHIFT_R(ROL_L_DX_DY, ROL_L_R)
#undef OP_ROR_B_DX_DY
#define OP_ROR_B_DX_DY PD_SHIFT_R(ROR_B_DX_DY, ROR_B_R)
#undef OP_ROR_W_DX_DY
#define OP_ROR_W_DX_DY PD_SHIFT_R(ROR_W_DX_DY, ROR_W_R)
#undef OP_ROR_L_DX_DY
#define OP_ROR_L_DX_DY PD_SHIFT_R(ROR_L_DX_DY, ROR_L_R)

// multiply / divide
#undef OP_MULU_W_EA_DN
#define OP_MULU_W_EA_DN return pd_cls_ea_dn(ctx, opcode, 16, PDF_MULU_W, true, PD_DATA_ALLOW, T1_MULU_W_EA_DN)
#undef OP_MULS_W_EA_DN
#define OP_MULS_W_EA_DN return pd_cls_ea_dn(ctx, opcode, 16, PDF_MULS_W, true, PD_DATA_ALLOW, T1_MULS_W_EA_DN)
#undef OP_DIVU_W_EA_DN
#define OP_DIVU_W_EA_DN return pd_cls_ea_dn(ctx, opcode, 16, PDF_DIVU_W, false, PD_DATA_ALLOW, T1_DIVU_W_EA_DN)
#undef OP_DIVS_W_EA_DN
#define OP_DIVS_W_EA_DN return pd_cls_ea_dn(ctx, opcode, 16, PDF_DIVS_W, false, PD_DATA_ALLOW, T1_DIVS_W_EA_DN)

// bit operations on Dn: immediate bit number (mod 32) or Dx
#undef OP_BTST_L_DATA_DN
#define OP_BTST_L_DATA_DN return ctx->avail < 2 ? T1_BTST_L_DATA_DN : pd_emit(ctx, PDF_BTST_I_D, PD_DOFF(PD_OP_REG(opcode)), (uint8_t)(pd_cw(ctx, 1) & 0x1Fu), 0, 2)
#undef OP_BCHG_L_DATA_DN
#define OP_BCHG_L_DATA_DN return ctx->avail < 2 ? T1_BCHG_L_DATA_DN : pd_emit(ctx, PDF_BCHG_I_D, PD_DOFF(PD_OP_REG(opcode)), (uint8_t)(pd_cw(ctx, 1) & 0x1Fu), 0, 2)
#undef OP_BCLR_L_DATA_DN
#define OP_BCLR_L_DATA_DN return ctx->avail < 2 ? T1_BCLR_L_DATA_DN : pd_emit(ctx, PDF_BCLR_I_D, PD_DOFF(PD_OP_REG(opcode)), (uint8_t)(pd_cw(ctx, 1) & 0x1Fu), 0, 2)
#undef OP_BSET_L_DATA_DN
#define OP_BSET_L_DATA_DN return ctx->avail < 2 ? T1_BSET_L_DATA_DN : pd_emit(ctx, PDF_BSET_I_D, PD_DOFF(PD_OP_REG(opcode)), (uint8_t)(pd_cw(ctx, 1) & 0x1Fu), 0, 2)
#undef OP_BTST_L_DX_DY
#define OP_BTST_L_DX_DY return pd_emit(ctx, PDF_BTST_D_D, PD_DOFF(PD_OP_REG(opcode)), PD_DOFF(PD_OP_DN9(opcode)), 0, 1)
#undef OP_BCHG_L_DX_DY
#define OP_BCHG_L_DX_DY return pd_emit(ctx, PDF_BCHG_D_D, PD_DOFF(PD_OP_REG(opcode)), PD_DOFF(PD_OP_DN9(opcode)), 0, 1)
#undef OP_BCLR_L_DX_DY
#define OP_BCLR_L_DX_DY return pd_emit(ctx, PDF_BCLR_D_D, PD_DOFF(PD_OP_REG(opcode)), PD_DOFF(PD_OP_DN9(opcode)), 0, 1)
#undef OP_BSET_L_DX_DY
#define OP_BSET_L_DX_DY return pd_emit(ctx, PDF_BSET_D_D, PD_DOFF(PD_OP_REG(opcode)), PD_DOFF(PD_OP_DN9(opcode)), 0, 1)

// CMPM / EXG / ABCD / SBCD
#undef OP_CMPM_B_AY_AX
#define OP_CMPM_B_AY_AX return (PD_OP_REG(opcode) == 7 || PD_OP_DN9(opcode) == 7) ? T1_CMPM_B_AY_AX : pd_emit(ctx, PDF_CMPM_B, PD_AOFF(PD_OP_DN9(opcode)), PD_AOFF(PD_OP_REG(opcode)), 0, 1)
#undef OP_CMPM_W_AY_AX
#define OP_CMPM_W_AY_AX return pd_emit(ctx, PDF_CMPM_W, PD_AOFF(PD_OP_DN9(opcode)), PD_AOFF(PD_OP_REG(opcode)), 0, 1)
#undef OP_CMPM_L_AY_AX
#define OP_CMPM_L_AY_AX return pd_emit(ctx, PDF_CMPM_L, PD_AOFF(PD_OP_DN9(opcode)), PD_AOFF(PD_OP_REG(opcode)), 0, 1)
#undef OP_EXG_DX_DY
#define OP_EXG_DX_DY return pd_emit(ctx, PDF_EXG_DD, PD_DOFF(PD_OP_DN9(opcode)), PD_DOFF(PD_OP_REG(opcode)), 0, 1)
#undef OP_EXG_AX_AY
#define OP_EXG_AX_AY return pd_emit(ctx, PDF_EXG_AA, PD_AOFF(PD_OP_DN9(opcode)), PD_AOFF(PD_OP_REG(opcode)), 0, 1)
#undef OP_EXG_DX_AY
#define OP_EXG_DX_AY return pd_emit(ctx, PDF_EXG_DA, PD_DOFF(PD_OP_DN9(opcode)), PD_AOFF(PD_OP_REG(opcode)), 0, 1)
#undef OP_ABCD_DY_DX
#define OP_ABCD_DY_DX return pd_emit(ctx, PDF_ABCD_DD, PD_DOFF(PD_OP_DN9(opcode)), PD_DOFF(PD_OP_REG(opcode)), 0, 1)
#undef OP_SBCD_DX_DY
#define OP_SBCD_DX_DY return pd_emit(ctx, PDF_SBCD_DD, PD_DOFF(PD_OP_DN9(opcode)), PD_DOFF(PD_OP_REG(opcode)), 0, 1)

// branches
#undef OP_BCC_B_DISPLACEMENT
#define OP_BCC_B_DISPLACEMENT return pd_cls_bcc(ctx, opcode, false, T1_BCC_B_DISPLACEMENT)
#undef OP_BCC_W_DISPLACEMENT
#define OP_BCC_W_DISPLACEMENT return pd_cls_bcc(ctx, opcode, true, T1_BCC_W_DISPLACEMENT)
#undef OP_BSR_B_LABEL
#define OP_BSR_B_LABEL return pd_cls_bcc(ctx, opcode, false, T1_BSR_B_LABEL)
#undef OP_BSR_W_LABEL
#define OP_BSR_W_LABEL return pd_cls_bcc(ctx, opcode, true, T1_BSR_W_LABEL)
#undef OP_DBCC_DN_LABEL
#define OP_DBCC_DN_LABEL return pd_cls_dbcc(ctx, opcode, T1_DBCC_DN_LABEL)
#undef OP_JMP_EA
#define OP_JMP_EA return pd_cls_control(ctx, opcode, PDF_JMP_IND, PDF_JMP_D16, PDF_JMP_ABS, 0, T1_JMP_EA)
#undef OP_JSR_EA
#define OP_JSR_EA return pd_cls_control(ctx, opcode, PDF_JSR_IND, PDF_JSR_D16, PDF_JSR_ABS, 0, T1_JSR_EA)
#undef OP_PEA_EA
#define OP_PEA_EA return pd_cls_control(ctx, opcode, PDF_PEA_IND, PDF_PEA_D16, PDF_PEA_ABS, 0, T1_PEA_EA)
#undef OP_RTS
#define OP_RTS return pd_emit(ctx, PDF_RTS, 0, 0, 0, 1)
#undef OP_NOP
#define OP_NOP return pd_emit(ctx, PDF_NOP, 0, 0, 0, 1)
#undef OP_UNLK
#define OP_UNLK return pd_emit(ctx, PDF_UNLK, PD_AOFF(PD_OP_REG(opcode)), 0, 0, 1)
#undef OP_LINK
#define OP_LINK return ctx->avail < 2 ? T1_LINK : pd_emit(ctx, PDF_LINK_W, PD_AOFF(PD_OP_REG(opcode)), 0, (uint32_t)(int32_t)(int16_t)pd_cw(ctx, 1), 2)
#undef OP_MOVEM_W_LIST_EA
#define OP_MOVEM_W_LIST_EA return pd_cls_movem(ctx, opcode, false, PDF_MOVEM_W_R_DEC, PDF_MOVEM_L_R_DEC, T1_MOVEM_W_LIST_EA)
#undef OP_MOVEM_L_LIST_EA
#define OP_MOVEM_L_LIST_EA return pd_cls_movem(ctx, opcode, false, PDF_MOVEM_W_R_DEC, PDF_MOVEM_L_R_DEC, T1_MOVEM_L_LIST_EA)
#undef OP_MOVEM_W_EA_LIST
#define OP_MOVEM_W_EA_LIST return pd_cls_movem(ctx, opcode, true, PDF_MOVEM_W_R_DEC, PDF_MOVEM_L_R_DEC, T1_MOVEM_W_EA_LIST)
#undef OP_MOVEM_L_EA_LIST
#define OP_MOVEM_L_EA_LIST return pd_cls_movem(ctx, opcode, true, PDF_MOVEM_W_R_DEC, PDF_MOVEM_L_R_DEC, T1_MOVEM_L_EA_LIST)
#undef OP_SCC_EA
#define OP_SCC_EA return PD_OP_MODE(opcode) != 0 ? T1_SCC_EA : pd_emit(ctx, PDF_SCC_D, PD_DOFF(PD_OP_REG(opcode)), (uint8_t)((opcode >> 8) & 0xFu), 0, 1)
#undef OP_ATRAP
#define OP_ATRAP return pd_emit(ctx, PDF_ATRAP, 0, 0, opcode, 1)
#undef OP_TRAP_VECTOR
#define OP_TRAP_VECTOR return pd_emit(ctx, PDF_TRAP, 0, (uint8_t)(opcode & 0xFu), 0, 1)

// LEA (the 030 tree routes EXTB.L Dn through the LEA leaf: mode 0, An field 4)
#ifdef CPU_DECODER_IS_68030
#undef OP_LEA_EA_AN
#define OP_LEA_EA_AN                                                                                                   \
    do {                                                                                                               \
        if (PD_OP_MODE(opcode) == 0)                                                                                   \
            return PD_OP_DN9(opcode) == 4 ? pd_emit(ctx, PDF_EXTB_L, PD_DOFF(PD_OP_REG(opcode)), 0, 0, 1)             \
                                          : T1_LEA_EA_AN;                                                              \
        return pd_cls_control(ctx, opcode, PDF_LEA_IND, PDF_LEA_D16, PDF_LEA_ABS, PD_AOFF(PD_OP_DN9(opcode)),          \
                              T1_LEA_EA_AN);                                                                           \
    } while (0)
#undef OP_RTD_DISPLACEMENT
#define OP_RTD_DISPLACEMENT return ctx->avail < 2 ? T1_RTD_DISPLACEMENT : pd_emit(ctx, PDF_RTD, 0, 0, (uint32_t)(int32_t)(int16_t)pd_cw(ctx, 1), 2)
#undef OP_LINK_L_AN_DISP
#define OP_LINK_L_AN_DISP return ctx->avail < 3 ? T1_LINK_L_AN_DISP : pd_emit(ctx, PDF_LINK_L, PD_AOFF(PD_OP_REG(opcode)), 0, pd_cl(ctx, 1), 3)
#else
#undef OP_LEA_EA_AN
#define OP_LEA_EA_AN return pd_cls_control(ctx, opcode, PDF_LEA_IND, PDF_LEA_D16, PDF_LEA_ABS, PD_AOFF(PD_OP_DN9(opcode)), T1_LEA_EA_AN)
#endif
// clang-format on

// The FPU leaf computes its own EAs and reaches into cpu->pc: always generic.
#undef OP_FPU_GENERAL
#define OP_FPU_GENERAL return PD_GENERIC

#define CPU_DECODER_NAME        PD_TREE_NAME
#define CPU_DECODER_ARGS        cpu_t *restrict cpu, uint16_t opcode, uint16_t ext_word, pd_cls_t *ctx
#define CPU_DECODER_RETURN_TYPE static uint16_t
#define CPU_DECODER_PROLOGUE                                                                                           \
    (void)cpu;                                                                                                         \
    (void)ext_word;                                                                                                    \
    (void)ctx
#define CPU_DECODER_EPILOGUE return PD_GENERIC
#include "cpu_decode.h"
#undef CPU_DECODER_NAME
#undef CPU_DECODER_ARGS
#undef CPU_DECODER_RETURN_TYPE
#undef CPU_DECODER_PROLOGUE
#undef CPU_DECODER_EPILOGUE

// Classify the word at idx.  T1 entries carry the opcode in c and the
// extension word in a:b (both must be readable: a leaf in the page's last
// word is PD_CROSS so its words are fetched through memory as today).
static uint16_t PD_CLASSIFY_NAME(cpu_t *restrict cpu, const uint8_t *host, uint32_t idx, uint32_t avail,
                                 uint32_t page_lo, pd_entry_t *e, uint32_t *len) {
    if (avail < 2)
        return PD_CROSS;
    uint16_t opcode = LOAD_BE16(host + (idx << 1));
    uint16_t ext_word = LOAD_BE16(host + ((idx + 1) << 1));
    e->c = opcode;
    e->a = (uint8_t)(ext_word >> 8);
    e->b = (uint8_t)ext_word;
    *len = 0;
    pd_cls_t ctx = {host, idx, avail, page_lo, e, len};
    return PD_TREE_NAME(cpu, opcode, ext_word, &ctx);
}
