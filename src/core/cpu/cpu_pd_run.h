// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_pd_run.h
// The 68K predecoded sprint loop (proposal §3.4-§3.5, §4): one flat switch
// over specialized (T0), flattened-generic (T1) and generic (T2) entries of
// the block the PC is executing from.  A template included once per core
// file after that core's cpu_ops.h bindings and its switch executor, so
// every T1 case runs the body the core binds to the leaf and every T0
// handler uses the same condition-code macros as the switch core (the raw
// flag words must be bit-identical for checkpoint equality).
//
// Includer contract (defined by cpu_68000.c / cpu_68030.c / cpu_68040.c):
//   PD_RUN_NAME        the sprint function to define (cpu_pd_run_68030)
//   PD_STEP_NAME       the one-instruction executor (defined here from cpu_decode.h)
//   PD_TREE_NAME       the classifier's tree instantiation name
//   PD_CLASSIFY_NAME   the classifier entry point (cpu_pd_classify.h, included last)
//   PD_HW_RESET(cpu)   030/040: the double-bus-fault reset routine
// plus the model macros CPU_DECODER_IS_68030 / CPU_DECODER_IS_68040.

#include "cpu_pd_ids.h"
#include "predecode.h"

#include "lisa_mmu.h"

// ============================================================================
// The one-instruction executor (tier 2): the unmodified decode tree wrapped
// in do { } while (0) so the bodies' `continue` still means "abandon".
// ============================================================================

#define CPU_DECODER_NAME        PD_STEP_NAME
#define CPU_DECODER_ARGS        cpu_t *restrict cpu, uint32_t *instructions, uint16_t opcode, uint16_t ext_word
#define CPU_DECODER_RETURN_TYPE static __attribute__((noinline)) void
#define CPU_DECODER_PROLOGUE                                                                                           \
    (void)instructions;                                                                                                \
    (void)ext_word;                                                                                                    \
    do {
#define CPU_DECODER_EPILOGUE                                                                                           \
    }                                                                                                                  \
    while (0)
#include "cpu_decode.h"
#undef CPU_DECODER_NAME
#undef CPU_DECODER_ARGS
#undef CPU_DECODER_RETURN_TYPE
#undef CPU_DECODER_PROLOGUE
#undef CPU_DECODER_EPILOGUE

// The classifier is instantiated after the loop (it rebinds the OP_ names).
static uint16_t PD_CLASSIFY_NAME(cpu_t *restrict cpu, const uint8_t *host, uint32_t idx, uint32_t avail,
                                 uint32_t page_lo, pd_entry_t *e, uint32_t *len);

// ============================================================================
// Decode one entry at first execution, with the flag-liveness pass (§5).
// ============================================================================

static __attribute__((noinline)) void PD_DECODE_NAME(cpu_t *restrict cpu, pd_block_t *blk, uint32_t idx,
                                                     uint32_t page_lo) {
    pd_entry_t e;
    uint32_t len = 0;
    uint16_t id = PD_CLASSIFY_NAME(cpu, blk->host, idx, PD_ENTRIES_68K - idx, page_lo, &e, &len);
    e.id = id;
    // The words this entry was decoded from: the opcode and its extension
    // words, so the debug audit can check every word a handler consumes.
    // T1 entries carry the opcode and its first extension word; T0 shapes
    // carry `len` words.
    for (uint32_t w = 0; w < (len > 2 ? len : 2) && idx + w < PD_ENTRIES_68K; w++)
        blk->raw16[idx + w] = LOAD_BE16(blk->host + ((idx + w) << 1));
    // Elision: retarget a definer whose NZVC result is dead on the sequential
    // path — the successor writes all four unconditionally, reads none, and
    // cannot fault or trap before writing them (§5.3 rule 2).  Level 1 keeps
    // memory-form definers at full flags; level 2 lets their twins run with
    // the slow-path guard (§5.3 rule 3).
    int level = predecode_elide_level();
    uint8_t prop = g_cpu_pd_prop[id];
    if (level > 0 && (prop & PD_P_ELIDABLE) && len != 0 && (level >= 2 || !(prop & PD_P_MEMDEF))) {
        uint32_t j = idx + len;
        if (j < PD_ENTRIES_68K) {
            pd_entry_t ej;
            uint32_t lj = 0;
            uint16_t jid = PD_CLASSIFY_NAME(cpu, blk->host, j, PD_ENTRIES_68K - j, page_lo, &ej, &lj);
            uint8_t pj = g_cpu_pd_prop[jid];
            if ((pj & PD_P_WNZVC) && !(pj & PD_P_CANFAULT)) {
                e.id = (uint16_t)(id + 1);
                g_pd_stats.elided++;
            }
        }
    }
    blk->e[idx] = e;
    predecode_count_decode(PD_ARCH_68K, id);
}

// ============================================================================
// Operand access by entry field
// ============================================================================

// Register file access by byte offset (a/b fields).
#define PD_R(off)            (*(uint32_t *)((uint8_t *)cpu + (off)))
#define PD_RD(bits, off)     ((UINT(bits))PD_R(off))
#define PD_STD8(off, v)      (PD_R(off) = (PD_R(off) & 0xFFFFFF00u) | (uint8_t)(v))
#define PD_STD16(off, v)     (PD_R(off) = (PD_R(off) & 0xFFFF0000u) | (uint16_t)(v))
#define PD_STD32(off, v)     (PD_R(off) = (uint32_t)(v))
#define PD_STD(bits, off, v) PD_STD##bits(off, v)
#define PD_SP                PD_R(offsetof(cpu_t, a) + 28)

// (An)+ and -(An) loads (calculate_ea semantics: the register moves before
// the read; a faulting read leaves it moved, as read_ea_* does today).
#define PD_DEF_LD_INC_DEC(bits)                                                                                        \
    static inline UINT(bits) pd_ld_inc##bits(cpu_t *restrict cpu, uint8_t off) {                                       \
        uint32_t ea = PD_R(off);                                                                                       \
        PD_R(off) = ea + (bits) / 8;                                                                                   \
        return READ##bits(ea);                                                                                         \
    }                                                                                                                  \
    static inline UINT(bits) pd_ld_dec##bits(cpu_t *restrict cpu, uint8_t off) {                                       \
        uint32_t ea = PD_R(off) - (bits) / 8;                                                                          \
        PD_R(off) = ea;                                                                                                \
        return READ##bits(ea);                                                                                         \
    }
PD_DEF_LD_INC_DEC(8)
PD_DEF_LD_INC_DEC(16)
PD_DEF_LD_INC_DEC(32)

// Destination stores with write_ea_* semantics: a fault already pending
// (an earlier access of the same instruction) aborts before the data cycle
// and restores An; a fault in the write itself restores An too.
#define PD_DEF_ST_INC_DEC(bits)                                                                                        \
    static inline void pd_st_inc##bits(cpu_t *restrict cpu, uint8_t off, UINT(bits) v) {                               \
        uint32_t saved = PD_R(off);                                                                                    \
        uint32_t ea = saved;                                                                                           \
        PD_R(off) = ea + (bits) / 8;                                                                                   \
        if (__builtin_expect(g_bus_error_pending, 0)) {                                                                \
            PD_R(off) = saved;                                                                                         \
            return;                                                                                                    \
        }                                                                                                              \
        WRITE##bits(ea, v);                                                                                            \
        if (__builtin_expect(g_bus_error_pending, 0))                                                                  \
            PD_R(off) = saved;                                                                                         \
    }                                                                                                                  \
    static inline void pd_st_dec##bits(cpu_t *restrict cpu, uint8_t off, UINT(bits) v) {                               \
        uint32_t saved = PD_R(off);                                                                                    \
        uint32_t ea = saved - (bits) / 8;                                                                              \
        PD_R(off) = ea;                                                                                                \
        if (__builtin_expect(g_bus_error_pending, 0)) {                                                                \
            PD_R(off) = saved;                                                                                         \
            return;                                                                                                    \
        }                                                                                                              \
        WRITE##bits(ea, v);                                                                                            \
        if (__builtin_expect(g_bus_error_pending, 0))                                                                  \
            PD_R(off) = saved;                                                                                         \
    }
PD_DEF_ST_INC_DEC(8)
PD_DEF_ST_INC_DEC(16)
PD_DEF_ST_INC_DEC(32)

// Source loads by shape.  b = register offset for D/IND/INC/DEC/D16; c = the
// displacement (low 16 bits), the absolute address, or the immediate.
#define PD_LD_D(bits)   PD_RD(bits, e.b)
#define PD_LD_IND(bits) READ##bits(PD_R(e.b))
#define PD_LD_INC(bits) pd_ld_inc##bits(cpu, e.b)
#define PD_LD_DEC(bits) pd_ld_dec##bits(cpu, e.b)
#define PD_LD_D16(bits) READ##bits(PD_R(e.b) + (uint32_t)(int32_t)(int16_t)e.c)
#define PD_LD_ABS(bits) READ##bits(e.c)
#define PD_LD_IMM(bits) ((UINT(bits))e.c)

// Read-modify-write loads by destination shape (READ_EA without update).
#define PD_RMW_D(bits, DISP)   PD_RD(bits, e.a)
#define PD_RMW_IND(bits, DISP) READ##bits(PD_R(e.a))
#define PD_RMW_INC(bits, DISP) READ##bits(PD_R(e.a))
#define PD_RMW_DEC(bits, DISP) READ##bits(PD_R(e.a) - (bits) / 8)
#define PD_RMW_D16(bits, DISP) READ##bits(PD_R(e.a) + (uint32_t)(DISP))
#define PD_RMW_ABS(bits, DISP) READ##bits(e.c)

// Destination stores by shape (write_ea_* semantics).
#define PD_ST_D(bits, v, DISP) PD_STD(bits, e.a, v)
#define PD_ST_IND(bits, v, DISP)                                                                                       \
    do {                                                                                                               \
        if (__builtin_expect(!g_bus_error_pending, 1))                                                                 \
            WRITE##bits(PD_R(e.a), v);                                                                                 \
    } while (0)
#define PD_ST_INC(bits, v, DISP) pd_st_inc##bits(cpu, e.a, v)
#define PD_ST_DEC(bits, v, DISP) pd_st_dec##bits(cpu, e.a, v)
#define PD_ST_D16(bits, v, DISP)                                                                                       \
    do {                                                                                                               \
        if (__builtin_expect(!g_bus_error_pending, 1))                                                                 \
            WRITE##bits(PD_R(e.a) + (uint32_t)(DISP), v);                                                              \
    } while (0)
#define PD_ST_ABS(bits, v, DISP)                                                                                       \
    do {                                                                                                               \
        if (__builtin_expect(!g_bus_error_pending, 1))                                                                 \
            WRITE##bits(e.c, v);                                                                                       \
    } while (0)

// Shape facts: does it touch memory; extension words; length in a field.
#define PD_MEM_D   0
#define PD_MEM_IND 1
#define PD_MEM_INC 1
#define PD_MEM_DEC 1
#define PD_MEM_D16 1
#define PD_MEM_ABS 1
#define PD_MEM_IMM 0
#define PD_XW_D    0
#define PD_XW_IND  0
#define PD_XW_INC  0
#define PD_XW_DEC  0
#define PD_XW_D16  1
#define PD_XW_ABS  0 // length carried in the entry (PD_VLEN_*)
#define PD_XW_IMM  0
// Source shapes whose length lives in b (ABS/IMM), destination shapes whose
// length lives in a (ABS).
#define PD_VLEN_S_D   0
#define PD_VLEN_S_IND 0
#define PD_VLEN_S_INC 0
#define PD_VLEN_S_DEC 0
#define PD_VLEN_S_D16 0
#define PD_VLEN_S_ABS 1
#define PD_VLEN_S_IMM 1
#define PD_VLEN_D_D   0
#define PD_VLEN_D_IND 0
#define PD_VLEN_D_INC 0
#define PD_VLEN_D_DEC 0
#define PD_VLEN_D_D16 0
#define PD_VLEN_D_ABS 1

// Length of an ea,Dn form by source shape / of a Dn,ea form by destination shape.
#define PD_LEN_S(SH) (PD_VLEN_S_##SH ? (uint32_t)e.b : (1u + PD_XW_##SH))
#define PD_LEN_D(SH) (PD_VLEN_D_##SH ? (uint32_t)e.a : (1u + PD_XW_##SH))
// Length of a MOVE by both shapes.
#define PD_LEN_MV(S, D) (PD_VLEN_S_##S ? (uint32_t)e.b : PD_VLEN_D_##D ? (uint32_t)e.a : (1u + PD_XW_##S + PD_XW_##D))

// The displacement of a (d16,An) destination: the high half when the
// source also needed c (MOVE (d16,An),(d16,An); #imm.B/W,(d16,An)), else c.
#define PD_DISP_LO      (int32_t) e.c
#define PD_DISP_HI      (int32_t)(int16_t)(e.c >> 16)
#define PD_MV_DDISP_D   PD_DISP_LO
#define PD_MV_DDISP_IND PD_DISP_LO
#define PD_MV_DDISP_INC PD_DISP_LO
#define PD_MV_DDISP_DEC PD_DISP_LO
#define PD_MV_DDISP_D16 PD_DISP_HI
#define PD_MV_DDISP_ABS PD_DISP_LO
#define PD_MV_DDISP_IMM PD_DISP_HI

// MOVE's source-An restart snapshot (MOVE/MOVEx: rolled back on any fault).
#define PD_SAVE_SRC_D
#define PD_SAVE_SRC_IND
#define PD_SAVE_SRC_INC uint32_t _sv = PD_R(e.b);
#define PD_SAVE_SRC_DEC uint32_t _sv = PD_R(e.b);
#define PD_SAVE_SRC_D16
#define PD_SAVE_SRC_ABS
#define PD_SAVE_SRC_IMM
#define PD_RESTORE_SRC_D
#define PD_RESTORE_SRC_IND
#define PD_RESTORE_SRC_INC                                                                                             \
    if (__builtin_expect(g_bus_error_pending, 0))                                                                      \
        PD_R(e.b) = _sv;
#define PD_RESTORE_SRC_DEC                                                                                             \
    if (__builtin_expect(g_bus_error_pending, 0))                                                                      \
        PD_R(e.b) = _sv;
#define PD_RESTORE_SRC_D16
#define PD_RESTORE_SRC_ABS
#define PD_RESTORE_SRC_IMM

// ============================================================================
// Handler plumbing
// ============================================================================

// Address of the instruction being dispatched, and PC materialization
// (§3.5): only handlers that can reach memory, raise, or branch store it.
#define PD_IPC ipc
#define PD_MAT(len)                                                                                                    \
    do {                                                                                                               \
        cpu->instruction_pc = ipc;                                                                                     \
        cpu->pc = ipc + 2u * (uint32_t)(len);                                                                          \
    } while (0)
#define PD_MAT_IF(cond, len)                                                                                           \
    do {                                                                                                               \
        if (cond)                                                                                                      \
            PD_MAT(len);                                                                                               \
    } while (0)

// Sequential advance / branch to an in-page entry / re-derive from cpu->pc.
#define PD_NEXT(len)                                                                                                   \
    do {                                                                                                               \
        cur += (len);                                                                                                  \
        goto top;                                                                                                      \
    } while (0)
#define PD_JUMP_IN(index)                                                                                              \
    do {                                                                                                               \
        cur = blk->e + (index);                                                                                        \
        goto top;                                                                                                      \
    } while (0)
#define PD_JUMP_PC(target)                                                                                             \
    do {                                                                                                               \
        cpu->pc = (target);                                                                                            \
        goto relookup;                                                                                                 \
    } while (0)

// The 68000 latches the instruction register on every fetched instruction
// (the Lisa OS reads it from the group-0 frame; the checkpoint blob carries
// it).  The raw shadow is the word the entry was decoded from.
#ifdef CPU_DECODER_IS_68030
#define PD_LATCH_IR() ((void)0)
#else
#define PD_LATCH_IR()                                                                                                  \
    do {                                                                                                               \
        cpu->ir = blk->raw16[cur - blk->e];                                                                            \
        cpu->ir_pc = ipc;                                                                                              \
    } while (0)
#endif

// Every T0/T1 body starts here: the ir latch, the debug audit, the
// slow-path snapshot for the E2 guard (dead code where unused).
#define PD_ENTER(words)                                                                                                \
    PD_LATCH_IR();                                                                                                     \
    PD_AUDIT_68K(blk, (uint32_t)(cur - blk->e), (words));                                                              \
    uint64_t _sp0 = g_mem_slowpath_count;                                                                              \
    (void)_sp0
// The E2 guard: recompute the flags after all if a slow path ran (§5.3 rule 3).
#define PD_FLM (g_mem_slowpath_count != _sp0)

// ============================================================================
// Operation bodies.  Each takes the loaded source and a flag-liveness
// expression FL (1 = full flags, 0 = elided, PD_FLM = E2 guard); X is
// written exactly where the architecture writes it.
// ============================================================================

#define PD_OP_ADD(bits, _d, _s, FL, STORE)                                                                             \
    UINT(bits) _r = (UINT(bits))((_d) + (_s));                                                                         \
    if (FL) {                                                                                                          \
        UPDATE_C_ADD(_d, _s, _r);                                                                                      \
        UPDATE_V_ADD(_d, _s, _r);                                                                                      \
        UPDATE_N(_r);                                                                                                  \
        UPDATE_Z(_r);                                                                                                  \
        CC_X = CC_C;                                                                                                   \
    } else {                                                                                                           \
        CC_X = (_r) < (_d);                                                                                            \
    }                                                                                                                  \
    STORE(bits, _r)
#define PD_OP_SUB(bits, _d, _s, FL, STORE)                                                                             \
    UINT(bits) _r = (UINT(bits))((_d) - (_s));                                                                         \
    if (FL) {                                                                                                          \
        UPDATE_C_SUB(_d, _s, _r);                                                                                      \
        UPDATE_V_SUB(_d, _s, _r);                                                                                      \
        UPDATE_N(_r);                                                                                                  \
        UPDATE_Z(_r);                                                                                                  \
        CC_X = CC_C;                                                                                                   \
    } else {                                                                                                           \
        CC_X = (_r) > (_d);                                                                                            \
    }                                                                                                                  \
    STORE(bits, _r)
#define PD_OP_LOGIC(bits, _d, _s, op, FL, STORE)                                                                       \
    UINT(bits) _r = (UINT(bits))((_d)op(_s));                                                                          \
    if (FL) {                                                                                                          \
        UPDATE_NZ_CLEAR_CV(_r);                                                                                        \
    }                                                                                                                  \
    STORE(bits, _r)
#define PD_OP_CMP(bits, _d, _s, FL)                                                                                    \
    if (FL) {                                                                                                          \
        UINT(bits) _r;                                                                                                 \
        GENERIC_SUB(_d, _s, _r);                                                                                       \
    }
#define PD_OP_AND(bits, _d, _s, FL, STORE) PD_OP_LOGIC(bits, _d, _s, &, FL, STORE)
#define PD_OP_OR(bits, _d, _s, FL, STORE)  PD_OP_LOGIC(bits, _d, _s, |, FL, STORE)
#define PD_OP_EOR(bits, _d, _s, FL, STORE) PD_OP_LOGIC(bits, _d, _s, ^, FL, STORE)

// --- ea,Dn (S7P families): a = Dn, source by shape ---
#define PD_B_ADD_EA_DN(bits, SRC, FL)                                                                                  \
    UINT(bits) _d = PD_RD(bits, e.a);                                                                                  \
    UINT(bits) _s = SRC;                                                                                               \
    PD_OP_ADD(bits, _d, _s, FL, PD_STORE_A)
#define PD_B_SUB_EA_DN(bits, SRC, FL)                                                                                  \
    UINT(bits) _s = SRC;                                                                                               \
    UINT(bits) _d = PD_RD(bits, e.a);                                                                                  \
    PD_OP_SUB(bits, _d, _s, FL, PD_STORE_A)
#define PD_B_AND_EA_DN(bits, SRC, FL)                                                                                  \
    UINT(bits) _d = PD_RD(bits, e.a);                                                                                  \
    UINT(bits) _s = SRC;                                                                                               \
    PD_OP_AND(bits, _d, _s, FL, PD_STORE_A)
#define PD_B_OR_EA_DN(bits, SRC, FL)                                                                                   \
    UINT(bits) _d = PD_RD(bits, e.a);                                                                                  \
    UINT(bits) _s = SRC;                                                                                               \
    PD_OP_OR(bits, _d, _s, FL, PD_STORE_A)
#define PD_B_CMP_EA_DN(bits, SRC, FL)                                                                                  \
    UINT(bits) _s = SRC;                                                                                               \
    UINT(bits) _d = PD_RD(bits, e.a);                                                                                  \
    (void)_d;                                                                                                          \
    (void)_s;                                                                                                          \
    PD_OP_CMP(bits, _d, _s, FL)
#define PD_B_TST(bits, SRC, FL)                                                                                        \
    UINT(bits) _s = SRC;                                                                                               \
    (void)_s;                                                                                                          \
    if (FL) {                                                                                                          \
        UPDATE_NZ_CLEAR_CV(_s);                                                                                        \
    }
#define PD_B_CMPA(bits, SRC, FL)                                                                                       \
    UINT(bits) _s = SRC;                                                                                               \
    (void)_s;                                                                                                          \
    if (FL) {                                                                                                          \
        uint32_t _r;                                                                                                   \
        GENERIC_SUB(PD_R(e.a), (uint32_t)(int32_t)(INT(bits))_s, _r);                                                  \
    }
#define PD_B_MULU(bits, SRC, FL)                                                                                       \
    uint16_t _s = SRC;                                                                                                 \
    uint16_t _d = (uint16_t)PD_R(e.a);                                                                                 \
    uint32_t _r = (uint32_t)(uint16_t)_s * (uint32_t)_d;                                                               \
    if (FL) {                                                                                                          \
        UPDATE_NZ_CLEAR_CV(_r);                                                                                        \
    }                                                                                                                  \
    PD_R(e.a) = _r
#define PD_B_MULS(bits, SRC, FL)                                                                                       \
    uint16_t _s = SRC;                                                                                                 \
    int16_t _d = (int16_t)PD_R(e.a);                                                                                   \
    int32_t _r = (int32_t)(int16_t)_s * (int32_t)_d;                                                                   \
    if (FL) {                                                                                                          \
        UPDATE_NZ_CLEAR_CV(_r);                                                                                        \
    }                                                                                                                  \
    PD_R(e.a) = (uint32_t)_r
#define PD_STORE_A(bits, v) PD_STD(bits, e.a, v)

// --- S7S singles ---
#define PD_B_MOVEA(bits, SRC) PD_R(e.a) = (uint32_t)(int32_t)(INT(bits))(SRC)
#define PD_B_ADDA(bits, SRC)  PD_R(e.a) += (uint32_t)(int32_t)(INT(bits))(SRC)
#define PD_B_SUBA(bits, SRC)  PD_R(e.a) -= (uint32_t)(int32_t)(INT(bits))(SRC)
// Division: the exception path reads CCR after CLEAR_NZVC, so the id is
// never elided; PC is materialized before the divide can raise.
#define PD_B_DIVU(bits, SRC)                                                                                           \
    uint16_t _divisor = SRC;                                                                                           \
    CLEAR_NZVC();                                                                                                      \
    if (!_divisor) {                                                                                                   \
        PD_MAT(_len);                                                                                                  \
        EXC_DIVIDE_BY_ZERO();                                                                                          \
        goto relookup;                                                                                                 \
    } else {                                                                                                           \
        uint32_t _quotient = PD_R(e.a) / (uint16_t)_divisor;                                                           \
        if (_quotient > UINT16_MAX) {                                                                                  \
            CC_V = CC_N = 1;                                                                                           \
        } else {                                                                                                       \
            uint32_t _remainder = PD_R(e.a) % (uint16_t)_divisor;                                                      \
            PD_R(e.a) = (_remainder << 16) | (_quotient & 0xFFFF);                                                     \
            CC_N = _quotient & 0x8000;                                                                                 \
            CC_Z = (_quotient == 0);                                                                                   \
        }                                                                                                              \
    }
#define PD_B_DIVS(bits, SRC)                                                                                           \
    uint16_t _divisor = SRC;                                                                                           \
    CLEAR_NZVC();                                                                                                      \
    if (!_divisor) {                                                                                                   \
        PD_MAT(_len);                                                                                                  \
        EXC_DIVIDE_BY_ZERO();                                                                                          \
        goto relookup;                                                                                                 \
    } else {                                                                                                           \
        int32_t _q = (int32_t)PD_R(e.a) / (int16_t)_divisor;                                                           \
        if (((int16_t)_divisor == -1 && (int32_t)PD_R(e.a) == INT32_MIN) || _q > INT16_MAX || _q < INT16_MIN) {        \
            CC_V = CC_N = 1;                                                                                           \
        } else {                                                                                                       \
            int32_t _remainder = (int32_t)PD_R(e.a) % (int16_t)_divisor;                                               \
            PD_R(e.a) = ((uint32_t)_remainder << 16) | ((uint32_t)_q & 0xFFFF);                                        \
            CC_N = _q & 0x8000;                                                                                        \
            CC_Z = (_q == 0);                                                                                          \
        }                                                                                                              \
    }

// --- Dn,ea read-modify-write (D6P families): b = Dn, destination by shape ---
#define PD_B_ADD_DN_EA(bits, DST, FL)                                                                                  \
    UINT(bits) _d = PD_RMW_##DST(bits, PD_DISP_LO);                                                                    \
    UINT(bits) _s = PD_RD(bits, e.b);                                                                                  \
    PD_OP_ADD(bits, _d, _s, FL, PD_STORE_DST_##DST)
#define PD_B_SUB_DN_EA(bits, DST, FL)                                                                                  \
    UINT(bits) _d = PD_RMW_##DST(bits, PD_DISP_LO);                                                                    \
    UINT(bits) _s = PD_RD(bits, e.b);                                                                                  \
    PD_OP_SUB(bits, _d, _s, FL, PD_STORE_DST_##DST)
#define PD_B_AND_DN_EA(bits, DST, FL)                                                                                  \
    UINT(bits) _d = PD_RMW_##DST(bits, PD_DISP_LO);                                                                    \
    UINT(bits) _s = PD_RD(bits, e.b);                                                                                  \
    PD_OP_AND(bits, _d, _s, FL, PD_STORE_DST_##DST)
#define PD_B_OR_DN_EA(bits, DST, FL)                                                                                   \
    UINT(bits) _d = PD_RMW_##DST(bits, PD_DISP_LO);                                                                    \
    UINT(bits) _s = PD_RD(bits, e.b);                                                                                  \
    PD_OP_OR(bits, _d, _s, FL, PD_STORE_DST_##DST)
#define PD_B_EOR_DN_EA(bits, DST, FL)                                                                                  \
    UINT(bits) _d = PD_RMW_##DST(bits, PD_DISP_LO);                                                                    \
    UINT(bits) _s = PD_RD(bits, e.b);                                                                                  \
    PD_OP_EOR(bits, _d, _s, FL, PD_STORE_DST_##DST)
// The stores by destination shape (displacement low/high per family).
#define PD_STORE_DST_D(bits, v)   PD_ST_D(bits, v, 0)
#define PD_STORE_DST_IND(bits, v) PD_ST_IND(bits, v, 0)
#define PD_STORE_DST_INC(bits, v) PD_ST_INC(bits, v, 0)
#define PD_STORE_DST_DEC(bits, v) PD_ST_DEC(bits, v, 0)
#define PD_STORE_DST_D16(bits, v) PD_ST_D16(bits, v, _disp)
#define PD_STORE_DST_ABS(bits, v) PD_ST_ABS(bits, v, 0)

// --- #imm,ea (D6P families): c = immediate (low 16 with the d16 above it) ---
#define PD_B_ADDI(bits, DST, FL)                                                                                       \
    UINT(bits) _s = (UINT(bits))e.c;                                                                                   \
    UINT(bits) _d = PD_RMW_##DST(bits, PD_DISP_HI);                                                                    \
    PD_OP_ADD(bits, _d, _s, FL, PD_STORE_DST_##DST)
#define PD_B_SUBI(bits, DST, FL)                                                                                       \
    UINT(bits) _s = (UINT(bits))e.c;                                                                                   \
    UINT(bits) _d = PD_RMW_##DST(bits, PD_DISP_HI);                                                                    \
    PD_OP_SUB(bits, _d, _s, FL, PD_STORE_DST_##DST)
#define PD_B_ANDI(bits, DST, FL)                                                                                       \
    UINT(bits) _s = (UINT(bits))e.c;                                                                                   \
    UINT(bits) _d = PD_RMW_##DST(bits, PD_DISP_HI);                                                                    \
    PD_OP_AND(bits, _d, _s, FL, PD_STORE_DST_##DST)
#define PD_B_ORI(bits, DST, FL)                                                                                        \
    UINT(bits) _s = (UINT(bits))e.c;                                                                                   \
    UINT(bits) _d = PD_RMW_##DST(bits, PD_DISP_HI);                                                                    \
    PD_OP_OR(bits, _d, _s, FL, PD_STORE_DST_##DST)
#define PD_B_EORI(bits, DST, FL)                                                                                       \
    UINT(bits) _s = (UINT(bits))e.c;                                                                                   \
    UINT(bits) _d = PD_RMW_##DST(bits, PD_DISP_HI);                                                                    \
    PD_OP_EOR(bits, _d, _s, FL, PD_STORE_DST_##DST)
// CMPI reads its operand with update ((An)+ / -(An) move like a source).
#define PD_B_CMPI(bits, DST, FL)                                                                                       \
    UINT(bits) _s = (UINT(bits))e.c;                                                                                   \
    UINT(bits) _d = PD_CMPI_LD_##DST(bits);                                                                            \
    (void)_d;                                                                                                          \
    (void)_s;                                                                                                          \
    PD_OP_CMP(bits, _d, _s, FL)
#define PD_CMPI_LD_D(bits)   PD_RD(bits, e.a)
#define PD_CMPI_LD_IND(bits) READ##bits(PD_R(e.a))
#define PD_CMPI_LD_INC(bits) pd_ld_inc##bits(cpu, e.a)
#define PD_CMPI_LD_DEC(bits) pd_ld_dec##bits(cpu, e.a)
#define PD_CMPI_LD_D16(bits) READ##bits(PD_R(e.a) + (uint32_t)PD_DISP_HI)
#define PD_CMPI_LD_ABS(bits) READ##bits(e.c)

// --- ADDQ/SUBQ #q,ea (D6P): b = q ---
#define PD_B_ADDQ(bits, DST, FL)                                                                                       \
    UINT(bits) _s = (UINT(bits))e.b;                                                                                   \
    UINT(bits) _d = PD_RMW_##DST(bits, PD_DISP_LO);                                                                    \
    PD_OP_ADD(bits, _d, _s, FL, PD_STORE_DST_##DST)
#define PD_B_SUBQ(bits, DST, FL)                                                                                       \
    UINT(bits) _s = (UINT(bits))e.b;                                                                                   \
    UINT(bits) _d = PD_RMW_##DST(bits, PD_DISP_LO);                                                                    \
    PD_OP_SUB(bits, _d, _s, FL, PD_STORE_DST_##DST)
// --- CLR ea (D6P) ---
// Flags before the store when they are live (a faulting store leaves them
// set, as on the 68000); the E2 twin (FL = PD_FLM) keeps its after-the-fact
// recompute, which is the only case where FL is not a constant.
#define PD_B_CLR(bits, DST, FL)                                                                                        \
    if ((FL) == 1) {                                                                                                   \
        CC_N = CC_V = CC_C = 0;                                                                                        \
        CC_Z = 1;                                                                                                      \
    }                                                                                                                  \
    PD_STORE_DST_##DST(bits, 0);                                                                                       \
    if ((FL) != 1 && (FL)) {                                                                                           \
        CC_N = CC_V = CC_C = 0;                                                                                        \
        CC_Z = 1;                                                                                                      \
    }

// --- MOVE (one family per destination shape, 7 source shapes) ---
#define PD_B_MOVE(bits, SRC, DST, FL)                                                                                  \
    PD_SAVE_SRC_##SRC UINT(bits) _s = PD_LD_##SRC(bits);                                                               \
    if ((FL) == 1) {                                                                                                   \
        UPDATE_NZ_CLEAR_CV(_s);                                                                                        \
    }                                                                                                                  \
    PD_ST_##DST(bits, _s, PD_MV_DDISP_##SRC);                                                                          \
    PD_RESTORE_SRC_##SRC if ((FL) != 1 && (FL)) {                                                                      \
        UPDATE_NZ_CLEAR_CV(_s);                                                                                        \
    }

// ============================================================================
// Case stamping
// ============================================================================

// One case: enter, materialize PC when memory is involved, run, advance.
#define PD_CASE(ID, MEM, LEN, BITS, BODY)                                                                              \
    case ID: {                                                                                                         \
        uint32_t _len = (LEN);                                                                                         \
        PD_ENTER(_len);                                                                                                \
        PD_MAT_IF(MEM, _len);                                                                                          \
        BODY;                                                                                                          \
        PD_NEXT(_len);                                                                                                 \
    }

// ea,Dn pairs: seven source shapes, full and no-flags twins.
#define PD_S7P_PAIR(FAM, SH, BITS, BODY)                                                                               \
    PD_CASE(PD_ID_S7P(FAM, PD_SH_##SH), PD_MEM_##SH, PD_LEN_S(SH), BITS, BODY(BITS, PD_LD_##SH(BITS), 1))              \
    PD_CASE(PD_ID_S7P(FAM, PD_SH_##SH) + 1, PD_MEM_##SH, PD_LEN_S(SH), BITS,                                           \
            BODY(BITS, PD_LD_##SH(BITS), PD_MEM_##SH ? PD_FLM : 0))
#define PD_S7P(FAM, BITS, BODY)                                                                                        \
    PD_S7P_PAIR(FAM, D, BITS, BODY)                                                                                    \
    PD_S7P_PAIR(FAM, IND, BITS, BODY)                                                                                  \
    PD_S7P_PAIR(FAM, INC, BITS, BODY)                                                                                  \
    PD_S7P_PAIR(FAM, DEC, BITS, BODY)                                                                                  \
    PD_S7P_PAIR(FAM, D16, BITS, BODY)                                                                                  \
    PD_S7P_PAIR(FAM, ABS, BITS, BODY)                                                                                  \
    PD_S7P_PAIR(FAM, IMM, BITS, BODY)

// ea,An singles.
#define PD_S7S_ONE(FAM, SH, BITS, BODY)                                                                                \
    PD_CASE(PD_ID_S7S(FAM, PD_SH_##SH), PD_MEM_##SH, PD_LEN_S(SH), BITS, BODY(BITS, PD_LD_##SH(BITS)))
#define PD_S7S(FAM, BITS, BODY)                                                                                        \
    PD_S7S_ONE(FAM, D, BITS, BODY)                                                                                     \
    PD_S7S_ONE(FAM, IND, BITS, BODY)                                                                                   \
    PD_S7S_ONE(FAM, INC, BITS, BODY)                                                                                   \
    PD_S7S_ONE(FAM, DEC, BITS, BODY)                                                                                   \
    PD_S7S_ONE(FAM, D16, BITS, BODY)                                                                                   \
    PD_S7S_ONE(FAM, ABS, BITS, BODY)                                                                                   \
    PD_S7S_ONE(FAM, IMM, BITS, BODY)

// Dn,ea / #imm,ea / ADDQ / CLR pairs: six destination shapes.  DISP names
// the (d16,An) displacement macro the family's stores use.
// LEN is the length expression: PD_LEN_D(SH) when it follows from the
// destination shape (Dn,ea / ADDQ / CLR), PD_LEN_B for the #imm,ea families
// whose classifier stores the total length in b.
#define PD_LEN_B(SH) ((uint32_t)e.b)
#define PD_D6P_PAIR(FAM, SH, BITS, BODY, DISP, LEN)                                                                    \
    PD_CASE(PD_ID_D6P(FAM, PD_SH_##SH), PD_MEM_##SH, LEN(SH), BITS, uint32_t _disp = (uint32_t)(DISP); (void)_disp;    \
            BODY(BITS, SH, 1))                                                                                         \
    PD_CASE(PD_ID_D6P(FAM, PD_SH_##SH) + 1, PD_MEM_##SH, LEN(SH), BITS, uint32_t _disp = (uint32_t)(DISP);             \
            (void)_disp; BODY(BITS, SH, PD_MEM_##SH ? PD_FLM : 0))
#define PD_D6P_LEN(FAM, BITS, BODY, DISP, LEN)                                                                         \
    PD_D6P_PAIR(FAM, D, BITS, BODY, DISP, LEN)                                                                         \
    PD_D6P_PAIR(FAM, IND, BITS, BODY, DISP, LEN)                                                                       \
    PD_D6P_PAIR(FAM, INC, BITS, BODY, DISP, LEN)                                                                       \
    PD_D6P_PAIR(FAM, DEC, BITS, BODY, DISP, LEN)                                                                       \
    PD_D6P_PAIR(FAM, D16, BITS, BODY, DISP, LEN)                                                                       \
    PD_D6P_PAIR(FAM, ABS, BITS, BODY, DISP, LEN)
#define PD_D6P(FAM, BITS, BODY, DISP) PD_D6P_LEN(FAM, BITS, BODY, DISP, PD_LEN_D)
#define PD_D6P_IMM(FAM, BITS, BODY)   PD_D6P_LEN(FAM, BITS, BODY, PD_DISP_HI, PD_LEN_B)

// MOVE: 6 destination families × 7 source shapes × 2.
#define PD_MV_PAIR(FAMD, S, D, BITS)                                                                                   \
    PD_CASE(PD_ID_S7P(FAMD, PD_SH_##S), PD_MEM_##S || PD_MEM_##D, PD_LEN_MV(S, D), BITS, PD_B_MOVE(BITS, S, D, 1))     \
    PD_CASE(PD_ID_S7P(FAMD, PD_SH_##S) + 1, PD_MEM_##S || PD_MEM_##D, PD_LEN_MV(S, D), BITS,                           \
            PD_B_MOVE(BITS, S, D, (PD_MEM_##S || PD_MEM_##D) ? PD_FLM : 0))
#define PD_MV_DST(FAMD, DSH, BITS)                                                                                     \
    PD_MV_PAIR(FAMD, D, DSH, BITS)                                                                                     \
    PD_MV_PAIR(FAMD, IND, DSH, BITS)                                                                                   \
    PD_MV_PAIR(FAMD, INC, DSH, BITS)                                                                                   \
    PD_MV_PAIR(FAMD, DEC, DSH, BITS)                                                                                   \
    PD_MV_PAIR(FAMD, D16, DSH, BITS)                                                                                   \
    PD_MV_PAIR(FAMD, ABS, DSH, BITS)                                                                                   \
    PD_MV_PAIR(FAMD, IMM, DSH, BITS)
#define PD_MV_SIZE(SZ, BITS)                                                                                           \
    PD_MV_DST(MOVE_##SZ##_D, D, BITS)                                                                                  \
    PD_MV_DST(MOVE_##SZ##_IND, IND, BITS)                                                                              \
    PD_MV_DST(MOVE_##SZ##_INC, INC, BITS)                                                                              \
    PD_MV_DST(MOVE_##SZ##_DEC, DEC, BITS)                                                                              \
    PD_MV_DST(MOVE_##SZ##_D16, D16, BITS)                                                                              \
    PD_MV_DST(MOVE_##SZ##_ABS, ABS, BITS)

// Register-only pairs (a = Dn).
#define PD_PAIR(FAM, BITS, BODY)                                                                                       \
    PD_CASE(PDF_##FAM, 0, 1, BITS, BODY(BITS, 1))                                                                      \
    PD_CASE(PDF_##FAM + 1, 0, 1, BITS, BODY(BITS, 0))

// --- register-only unary bodies ---
#define PD_B_NEG(bits, FL)                                                                                             \
    UINT(bits) _d = PD_RD(bits, e.a);                                                                                  \
    UINT(bits) _r = (UINT(bits))(0 - _d);                                                                              \
    PD_STD(bits, e.a, _r);                                                                                             \
    if (FL) {                                                                                                          \
        UPDATE_Z(_r);                                                                                                  \
        UPDATE_N(_r);                                                                                                  \
        CC_C = CC_X = (_d > 0);                                                                                        \
        CC_V = (_d & _r) >> (bits - 1) & 1;                                                                            \
    } else {                                                                                                           \
        CC_X = (_d > 0);                                                                                               \
    }
#define PD_B_NOT(bits, FL)                                                                                             \
    UINT(bits) _d = PD_RD(bits, e.a);                                                                                  \
    _d = (UINT(bits)) ~_d;                                                                                             \
    if (FL) {                                                                                                          \
        UPDATE_NZ_CLEAR_CV(_d);                                                                                        \
    }                                                                                                                  \
    PD_STD(bits, e.a, _d)
#define PD_B_EXT_W(bits, FL)                                                                                           \
    uint16_t _r = (uint16_t)S_EXT_8TO16(PD_R(e.a));                                                                    \
    PD_STD16(e.a, _r);                                                                                                 \
    if (FL) {                                                                                                          \
        UPDATE_NZ_CLEAR_CV(_r);                                                                                        \
    }
#define PD_B_EXT_L(bits, FL)                                                                                           \
    uint32_t _r = (uint32_t)S_EXT_16TO32(PD_R(e.a));                                                                   \
    PD_R(e.a) = _r;                                                                                                    \
    if (FL) {                                                                                                          \
        UPDATE_NZ_CLEAR_CV(_r);                                                                                        \
    }
#define PD_B_EXTB_L(bits, FL)                                                                                          \
    uint32_t _r = (uint32_t)(int32_t)(int8_t)PD_R(e.a);                                                                \
    PD_R(e.a) = _r;                                                                                                    \
    if (FL) {                                                                                                          \
        UPDATE_NZ_CLEAR_CV(_r);                                                                                        \
    }
#define PD_B_SWAP(bits, FL)                                                                                            \
    PD_R(e.a) = (PD_R(e.a) >> 16) | (PD_R(e.a) << 16);                                                                 \
    if (FL) {                                                                                                          \
        UPDATE_NZ_CLEAR_CV(PD_R(e.a));                                                                                 \
    }
#define PD_B_MOVEQ(bits, FL)                                                                                           \
    uint32_t _r = e.c;                                                                                                 \
    PD_R(e.a) = _r;                                                                                                    \
    if (FL) {                                                                                                          \
        UPDATE_NZ_CLEAR_CV(_r);                                                                                        \
    }

// --- shifts and rotates (SHIFT_RIGHT / ASHIFT_LEFT / LSHIFT_LEFT / ROTATE_*
//     of cpu_ops.h with the register by offset; the count is the immediate
//     in b or Dx & 63 through b) ---
#define PD_CNT_I ((uint32_t)e.b)
#define PD_CNT_R (PD_R(e.b) & 0x3Fu)
#define PD_B_SHR(bits, CNT, OPX, FL)                                                                                   \
    UINT(bits) d = PD_RD(bits, e.a);                                                                                   \
    UINT(bits) c = (UINT(bits))(CNT);                                                                                  \
    UINT(bits) r = (UINT(bits))(OPX);                                                                                  \
    PD_STD(bits, e.a, r);                                                                                              \
    if (FL) {                                                                                                          \
        UPDATE_C_SHIFT_R(d, c, r);                                                                                     \
        UPDATE_X_SHIFT(c);                                                                                             \
        UPDATE_NZ_CLEAR_V(r);                                                                                          \
    } else {                                                                                                           \
        uint32_t _cc = c && (c > BITS(d) ? r : d & 1u << (c - 1));                                                     \
        CC_X = (c && _cc) || (!c && CC_X);                                                                             \
    }
#define PD_B_ASL(bits, CNT, OPX, FL)                                                                                   \
    UINT(bits) d = PD_RD(bits, e.a);                                                                                   \
    UINT(bits) c = (UINT(bits))(CNT);                                                                                  \
    UINT(bits) r = (UINT(bits))(OPX);                                                                                  \
    PD_STD(bits, e.a, r);                                                                                              \
    if (FL) {                                                                                                          \
        UPDATE_C_SHIFT_L(d, c);                                                                                        \
        UPDATE_X_SHIFT(c);                                                                                             \
        UPDATE_N(r);                                                                                                   \
        UPDATE_Z(r);                                                                                                   \
        CC_V = (!r && d) || (UINT(bits))((INT(bits))(1u << (bits - 1) & d) >> c ^ d) >> (bits - c - 1);                \
    } else {                                                                                                           \
        uint32_t _cc = c && (c <= BITS(d)) && (d & 1u << (BITS(d) - c));                                               \
        CC_X = (c && _cc) || (!c && CC_X);                                                                             \
    }
#define PD_B_LSL(bits, CNT, OPX, FL)                                                                                   \
    UINT(bits) d = PD_RD(bits, e.a);                                                                                   \
    UINT(bits) c = (UINT(bits))(CNT);                                                                                  \
    UINT(bits) r = (UINT(bits))(OPX);                                                                                  \
    PD_STD(bits, e.a, r);                                                                                              \
    if (FL) {                                                                                                          \
        UPDATE_C_SHIFT_L(d, c);                                                                                        \
        UPDATE_X_SHIFT(c);                                                                                             \
        UPDATE_NZ_CLEAR_V(r);                                                                                          \
    } else {                                                                                                           \
        uint32_t _cc = c && (c <= BITS(d)) && (d & 1u << (BITS(d) - c));                                               \
        CC_X = (c && _cc) || (!c && CC_X);                                                                             \
    }
#define PD_B_ROT(bits, CNT, SHIFT_EXPR, CARRY_EXPR, FL)                                                                \
    UINT(bits) d = PD_RD(bits, e.a);                                                                                   \
    UINT(bits) c = (UINT(bits))(CNT);                                                                                  \
    UINT(bits) s = c & ((bits) - 1);                                                                                   \
    UINT(bits) r = (UINT(bits))(SHIFT_EXPR);                                                                           \
    (void)s;                                                                                                           \
    PD_STD(bits, e.a, r);                                                                                              \
    if (FL) {                                                                                                          \
        CC_C = c ? (CARRY_EXPR) : 0;                                                                                   \
        UPDATE_NZ_CLEAR_V(r);                                                                                          \
    }
#define PD_ROR_EXPR(bits)  ((d >> s) | (d << ((bits - s) & ((bits) - 1))))
#define PD_ROL_EXPR(bits)  ((d << s) | (d >> ((bits - s) & ((bits) - 1))))
#define PD_ROR_CARRY(bits) ((r >> ((bits) - 1)) & 1)
#define PD_ROL_CARRY(bits) (r & 1)
// The per-size shift expressions, as the switch core spells them.
#define PD_ASR_EXPR8  ((int8_t)d >> MIN(c, 7))
#define PD_ASR_EXPR16 ((int16_t)d >> MIN(c, 15))
#define PD_ASR_EXPR32 ((int32_t)d >> MIN(c, 31))
#define PD_LSR_EXPR8  (c > 7 ? 0 : d >> c)
#define PD_LSR_EXPR16 (c > 15 ? 0 : d >> c)
#define PD_LSR_EXPR32 (c > 31 ? 0 : d >> c)
#define PD_SL_EXPR8   (c < 8 ? d << c : 0)
#define PD_SL_EXPR16  (c < 16 ? d << c : 0)
#define PD_SL_EXPR32  (c < 32 ? d << c : 0)
#define PD_SHIFT_PAIRS(SZ, BITS)                                                                                       \
    PD_CASE(PDF_ASR_##SZ##_I, 0, 1, BITS, PD_B_SHR(BITS, PD_CNT_I, PD_ASR_EXPR##BITS, 1))                              \
    PD_CASE(PDF_ASR_##SZ##_I + 1, 0, 1, BITS, PD_B_SHR(BITS, PD_CNT_I, PD_ASR_EXPR##BITS, 0))                          \
    PD_CASE(PDF_ASR_##SZ##_R, 0, 1, BITS, PD_B_SHR(BITS, PD_CNT_R, PD_ASR_EXPR##BITS, 1))                              \
    PD_CASE(PDF_ASR_##SZ##_R + 1, 0, 1, BITS, PD_B_SHR(BITS, PD_CNT_R, PD_ASR_EXPR##BITS, 0))                          \
    PD_CASE(PDF_LSR_##SZ##_I, 0, 1, BITS, PD_B_SHR(BITS, PD_CNT_I, PD_LSR_EXPR##BITS, 1))                              \
    PD_CASE(PDF_LSR_##SZ##_I + 1, 0, 1, BITS, PD_B_SHR(BITS, PD_CNT_I, PD_LSR_EXPR##BITS, 0))                          \
    PD_CASE(PDF_LSR_##SZ##_R, 0, 1, BITS, PD_B_SHR(BITS, PD_CNT_R, PD_LSR_EXPR##BITS, 1))                              \
    PD_CASE(PDF_LSR_##SZ##_R + 1, 0, 1, BITS, PD_B_SHR(BITS, PD_CNT_R, PD_LSR_EXPR##BITS, 0))                          \
    PD_CASE(PDF_ASL_##SZ##_I, 0, 1, BITS, PD_B_ASL(BITS, PD_CNT_I, PD_SL_EXPR##BITS, 1))                               \
    PD_CASE(PDF_ASL_##SZ##_I + 1, 0, 1, BITS, PD_B_ASL(BITS, PD_CNT_I, PD_SL_EXPR##BITS, 0))                           \
    PD_CASE(PDF_ASL_##SZ##_R, 0, 1, BITS, PD_B_ASL(BITS, PD_CNT_R, PD_SL_EXPR##BITS, 1))                               \
    PD_CASE(PDF_ASL_##SZ##_R + 1, 0, 1, BITS, PD_B_ASL(BITS, PD_CNT_R, PD_SL_EXPR##BITS, 0))                           \
    PD_CASE(PDF_LSL_##SZ##_I, 0, 1, BITS, PD_B_LSL(BITS, PD_CNT_I, PD_SL_EXPR##BITS, 1))                               \
    PD_CASE(PDF_LSL_##SZ##_I + 1, 0, 1, BITS, PD_B_LSL(BITS, PD_CNT_I, PD_SL_EXPR##BITS, 0))                           \
    PD_CASE(PDF_LSL_##SZ##_R, 0, 1, BITS, PD_B_LSL(BITS, PD_CNT_R, PD_SL_EXPR##BITS, 1))                               \
    PD_CASE(PDF_LSL_##SZ##_R + 1, 0, 1, BITS, PD_B_LSL(BITS, PD_CNT_R, PD_SL_EXPR##BITS, 0))                           \
    PD_CASE(PDF_ROR_##SZ##_I, 0, 1, BITS, PD_B_ROT(BITS, PD_CNT_I, PD_ROR_EXPR(BITS), PD_ROR_CARRY(BITS), 1))          \
    PD_CASE(PDF_ROR_##SZ##_I + 1, 0, 1, BITS, PD_B_ROT(BITS, PD_CNT_I, PD_ROR_EXPR(BITS), PD_ROR_CARRY(BITS), 0))      \
    PD_CASE(PDF_ROR_##SZ##_R, 0, 1, BITS, PD_B_ROT(BITS, PD_CNT_R, PD_ROR_EXPR(BITS), PD_ROR_CARRY(BITS), 1))          \
    PD_CASE(PDF_ROR_##SZ##_R + 1, 0, 1, BITS, PD_B_ROT(BITS, PD_CNT_R, PD_ROR_EXPR(BITS), PD_ROR_CARRY(BITS), 0))      \
    PD_CASE(PDF_ROL_##SZ##_I, 0, 1, BITS, PD_B_ROT(BITS, PD_CNT_I, PD_ROL_EXPR(BITS), PD_ROL_CARRY(BITS), 1))          \
    PD_CASE(PDF_ROL_##SZ##_I + 1, 0, 1, BITS, PD_B_ROT(BITS, PD_CNT_I, PD_ROL_EXPR(BITS), PD_ROL_CARRY(BITS), 0))      \
    PD_CASE(PDF_ROL_##SZ##_R, 0, 1, BITS, PD_B_ROT(BITS, PD_CNT_R, PD_ROL_EXPR(BITS), PD_ROL_CARRY(BITS), 1))          \
    PD_CASE(PDF_ROL_##SZ##_R + 1, 0, 1, BITS, PD_B_ROT(BITS, PD_CNT_R, PD_ROL_EXPR(BITS), PD_ROL_CARRY(BITS), 0))

// --- bit operations on Dn ---
#define PD_B_BTST(bits, BIT, FL)                                                                                       \
    uint32_t _mask = 1u << (BIT);                                                                                      \
    (void)_mask;                                                                                                       \
    if (FL) {                                                                                                          \
        CC_Z = !(PD_R(e.a) & _mask);                                                                                   \
    }
#define PD_B_BITOP(bits, BIT, OPX, FL)                                                                                 \
    uint32_t _mask = 1u << (BIT);                                                                                      \
    uint32_t _dst = PD_R(e.a);                                                                                         \
    if (FL) {                                                                                                          \
        CC_Z = !(_dst & _mask);                                                                                        \
    }                                                                                                                  \
    PD_R(e.a) = (OPX)
#define PD_BIT_I ((uint32_t)e.b)
#define PD_BIT_D (PD_R(e.b) & 0x1Fu)

// --- CMPM (Ay)+,(Ax)+: a = Ax, b = Ay ---
#define PD_B_CMPM(bits, FL)                                                                                            \
    uint32_t _sv_ay = PD_R(e.b), _sv_ax = PD_R(e.a);                                                                   \
    UINT(bits) _s = READ##bits(PD_R(e.b));                                                                             \
    PD_R(e.b) += (bits) / 8;                                                                                           \
    UINT(bits) _d = READ##bits(PD_R(e.a));                                                                             \
    PD_R(e.a) += (bits) / 8;                                                                                           \
    if (__builtin_expect(g_bus_error_pending, 0)) {                                                                    \
        PD_R(e.b) = _sv_ay;                                                                                            \
        PD_R(e.a) = _sv_ax;                                                                                            \
    }                                                                                                                  \
    (void)_d;                                                                                                          \
    (void)_s;                                                                                                          \
    PD_OP_CMP(bits, _d, _s, FL)

// --- MOVEM helpers: the staged, restart-safe loops of cpu_internal.h with
//     the mask and the resolved address passed in ---
static inline void pd_movem_to_regs(cpu_t *restrict cpu, uint16_t mask, uint32_t ea, int bits, int postinc_off) {
    uint32_t new_d[8], new_a[8];
    uint8_t d_set = 0, a_set = 0;
    for (int i = 0; i < 8; i++)
        if (mask & (1 << i)) {
            uint32_t v = bits == 16 ? (uint32_t)(int32_t)(int16_t)READ16(ea) : READ32(ea);
            ea += (uint32_t)bits >> 3;
            if (g_bus_error_pending)
                return;
            new_d[i] = v;
            d_set |= (uint8_t)(1 << i);
        }
    for (int i = 0; i < 8; i++)
        if (mask & (0x100 << i)) {
            uint32_t v = bits == 16 ? (uint32_t)(int32_t)(int16_t)READ16(ea) : READ32(ea);
            ea += (uint32_t)bits >> 3;
            if (g_bus_error_pending)
                return;
            new_a[i] = v;
            a_set |= (uint8_t)(1 << i);
        }
    for (int i = 0; i < 8; i++)
        if (d_set & (1 << i))
            cpu->d[i] = new_d[i];
    for (int i = 0; i < 8; i++)
        if (a_set & (1 << i))
            cpu->a[i] = new_a[i];
    if (postinc_off >= 0)
        PD_R((uint8_t)postinc_off) = ea; // (An)+ lands past the last register
}

static inline void pd_movem_from_regs_predec(cpu_t *restrict cpu, uint16_t mask, uint8_t an_off, int bits) {
    int step = bits >> 3;
    int an = (an_off - (int)offsetof(cpu_t, a)) / 4;
    uint32_t addr = cpu->a[an];
    for (int i = 0; i < 8; i++)
        if (mask & (1 << i)) {
            addr -= (uint32_t)step;
            uint32_t val = cpu->a[7 - i];
            if (cpu->cpu_model >= CPU_MODEL_68030 && (7 - i) == an)
                val -= (uint32_t)step; // 030+: An-step when An is the base
            if (bits == 16)
                WRITE16(addr, (uint16_t)val);
            else
                WRITE32(addr, val);
            if (g_bus_error_pending)
                return; // leave An unchanged for the retry
        }
    for (int i = 0; i < 8; i++)
        if (mask & (0x100 << i)) {
            addr -= (uint32_t)step;
            if (bits == 16)
                WRITE16(addr, (uint16_t)cpu->d[7 - i]);
            else
                WRITE32(addr, cpu->d[7 - i]);
            if (g_bus_error_pending)
                return;
        }
    cpu->a[an] = addr;
}

static inline void pd_movem_from_regs(cpu_t *restrict cpu, uint16_t mask, uint32_t ea, int bits) {
    int step = bits >> 3;
    for (int i = 0; i < 8; i++)
        if (mask & (1 << i)) {
            if (bits == 16)
                WRITE16(ea, (uint16_t)cpu->d[i]);
            else
                WRITE32(ea, cpu->d[i]);
            ea += (uint32_t)step;
            if (g_bus_error_pending)
                return;
        }
    for (int i = 0; i < 8; i++)
        if (mask & (0x100 << i)) {
            if (bits == 16)
                WRITE16(ea, (uint16_t)cpu->a[i]);
            else
                WRITE32(ea, cpu->a[i]);
            ea += (uint32_t)step;
            if (g_bus_error_pending)
                return;
        }
}

// PUSH with the write-first ordering of cpu_ops.h.
#define PD_PUSH32(v)                                                                                                   \
    do {                                                                                                               \
        WRITE32(PD_SP - 4, (v));                                                                                       \
        if (__builtin_expect(!g_bus_error_pending, 1))                                                                 \
            PD_SP -= 4;                                                                                                \
    } while (0)

// Bcc by condition: one _IN and one _OUT case each; cond 0 is BRA.
#define PD_BCC_CASES(SZ, COND)                                                                                         \
    case PDF_BCC_##SZ##_IN + (COND): {                                                                                 \
        PD_ENTER(e.a);                                                                                                 \
        if (conditional_test(cpu, (COND)))                                                                             \
            PD_JUMP_IN(e.c);                                                                                           \
        PD_NEXT(e.a);                                                                                                  \
    }                                                                                                                  \
    case PDF_BCC_##SZ##_OUT + (COND): {                                                                                \
        PD_ENTER(e.a);                                                                                                 \
        if (conditional_test(cpu, (COND)))                                                                             \
            PD_JUMP_PC(e.c);                                                                                           \
        PD_NEXT(e.a);                                                                                                  \
    }
#define PD_BCC_ALL(SZ)                                                                                                 \
    PD_BCC_CASES(SZ, 0)                                                                                                \
    PD_BCC_CASES(SZ, 2)                                                                                                \
    PD_BCC_CASES(SZ, 3)                                                                                                \
    PD_BCC_CASES(SZ, 4)                                                                                                \
    PD_BCC_CASES(SZ, 5)                                                                                                \
    PD_BCC_CASES(SZ, 6)                                                                                                \
    PD_BCC_CASES(SZ, 7)                                                                                                \
    PD_BCC_CASES(SZ, 8)                                                                                                \
    PD_BCC_CASES(SZ, 9)                                                                                                \
    PD_BCC_CASES(SZ, 10)                                                                                               \
    PD_BCC_CASES(SZ, 11)                                                                                               \
    PD_BCC_CASES(SZ, 12)                                                                                               \
    PD_BCC_CASES(SZ, 13)                                                                                               \
    PD_BCC_CASES(SZ, 14)                                                                                               \
    PD_BCC_CASES(SZ, 15)

// The T1 case body: materialize PC, bind opcode/ext_word, run the leaf.
#define PD_T1_BODY(OPX)                                                                                                \
    do {                                                                                                               \
        uint16_t opcode = (uint16_t)e.c;                                                                               \
        uint16_t ext_word = (uint16_t)(((uint16_t)e.a << 8) | e.b);                                                    \
        (void)ext_word;                                                                                                \
        (void)opcode;                                                                                                  \
        PD_ENTER(2);                                                                                                   \
        cpu->instruction_pc = ipc;                                                                                     \
        cpu->pc = ipc + 2;                                                                                             \
        OPX;                                                                                                           \
    } while (0)

// ============================================================================
// The sprint loop
// ============================================================================

void PD_RUN_NAME(cpu_t *restrict cpu, uint32_t *instructions) {
    // --- sprint entry: the switch core's prologue minus the loop ---
#ifdef CPU_DECODER_IS_68030
    if (__builtin_expect(cpu->halted, 0)) {
        cpu->halted = 0;
        PD_HW_RESET(cpu);
    }
    g_active_read = cpu->supervisor ? g_supervisor_read : g_user_read;
    g_active_write = cpu->supervisor ? g_supervisor_write : g_user_write;
#endif
    cpu_check_interrupt(cpu);
    g_bus_error_instr_ptr = instructions; // let memory slow paths force exit
#ifdef CPU_DECODER_IS_68030
    uint32_t _saved_trace = cpu->trace;
    if (__builtin_expect(_saved_trace & 2, 0))
        if (*instructions > 1)
            *instructions = 1;
#endif
    pd_block_t *blk = NULL; // the block of the page being executed (NULL: generic tier)
    bool pd_held = false; // the current uncached page was declined by the pool (stay generic until it changes)
    pd_entry_t *cur = NULL; // the entry to dispatch next
    uint32_t page_lo = 1; // guest address of the page (odd: no page yet)
    uint32_t ipc = cpu->instruction_pc; // address of the instruction being dispatched
    bool pd_slow = false; // last_bus_error_pc tracking wants the full prologue (§10)
    goto relookup;

top:
    if (*instructions == 0)
        goto done;
    ipc = page_lo + ((uint32_t)(cur - blk->e) << 1);
    if (__builtin_expect(pd_slow, 0)) {
        g_pd_stats.generic_slowmode++;
        cpu->pc = ipc;
        goto t2_step;
    }
    (*instructions)--;
    {
        pd_entry_t e = *cur;
        uint16_t id = e.id;
        // The last slot is never elided (§5.3 rule 1): the twin's full-flags
        // partner runs instead, so every sprint boundary sees exact CCR.
        if (__builtin_expect(*instructions == 0, 0) && (g_cpu_pd_prop[id] & PD_P_TWIN))
            id--;
    redispatch:
        switch (id) {
        case PD_UNDECODED:
            PD_DECODE_NAME(cpu, blk, (uint32_t)(cur - blk->e), page_lo);
            e = *cur;
            id = e.id;
            if (*instructions == 0 && (g_cpu_pd_prop[id] & PD_P_TWIN))
                id--;
            goto redispatch;

        case PD_CROSS:
            g_pd_stats.generic_cross++;
            (*instructions)++;
            cpu->pc = ipc;
            goto t2_step;
        case PD_GENERIC:
            // Generic tier: give the slot back, the T2 path takes it after its fetch.
            g_pd_stats.generic_declined++;
            (*instructions)++;
            cpu->pc = ipc;
            goto t2_step;

        case PD_PAGE_END:
            // Fell off the page (a one-word instruction in the last slot):
            // no instruction ran; continue on the next page.
            (*instructions)++;
            cpu->pc = ipc;
            goto relookup;

            // ---- T0: MOVE ----
            PD_MV_SIZE(B, 8)
            PD_MV_SIZE(W, 16)
            PD_MV_SIZE(L, 32)
            PD_S7S(MOVEA_W, 16, PD_B_MOVEA)
            PD_S7S(MOVEA_L, 32, PD_B_MOVEA)
            PD_PAIR(MOVEQ, 32, PD_B_MOVEQ)

            // ---- T0: ea,Dn ----
            PD_S7P(ADD_B_EA_DN, 8, PD_B_ADD_EA_DN)
            PD_S7P(ADD_W_EA_DN, 16, PD_B_ADD_EA_DN)
            PD_S7P(ADD_L_EA_DN, 32, PD_B_ADD_EA_DN)
            PD_S7P(SUB_B_EA_DN, 8, PD_B_SUB_EA_DN)
            PD_S7P(SUB_W_EA_DN, 16, PD_B_SUB_EA_DN)
            PD_S7P(SUB_L_EA_DN, 32, PD_B_SUB_EA_DN)
            PD_S7P(AND_B_EA_DN, 8, PD_B_AND_EA_DN)
            PD_S7P(AND_W_EA_DN, 16, PD_B_AND_EA_DN)
            PD_S7P(AND_L_EA_DN, 32, PD_B_AND_EA_DN)
            PD_S7P(OR_B_EA_DN, 8, PD_B_OR_EA_DN)
            PD_S7P(OR_W_EA_DN, 16, PD_B_OR_EA_DN)
            PD_S7P(OR_L_EA_DN, 32, PD_B_OR_EA_DN)
            PD_S7P(CMP_B_EA_DN, 8, PD_B_CMP_EA_DN)
            PD_S7P(CMP_W_EA_DN, 16, PD_B_CMP_EA_DN)
            PD_S7P(CMP_L_EA_DN, 32, PD_B_CMP_EA_DN)

            // ---- T0: Dn,ea ----
            PD_D6P(ADD_B_DN_EA, 8, PD_B_ADD_DN_EA, PD_DISP_LO)
            PD_D6P(ADD_W_DN_EA, 16, PD_B_ADD_DN_EA, PD_DISP_LO)
            PD_D6P(ADD_L_DN_EA, 32, PD_B_ADD_DN_EA, PD_DISP_LO)
            PD_D6P(SUB_B_DN_EA, 8, PD_B_SUB_DN_EA, PD_DISP_LO)
            PD_D6P(SUB_W_DN_EA, 16, PD_B_SUB_DN_EA, PD_DISP_LO)
            PD_D6P(SUB_L_DN_EA, 32, PD_B_SUB_DN_EA, PD_DISP_LO)
            PD_D6P(AND_B_DN_EA, 8, PD_B_AND_DN_EA, PD_DISP_LO)
            PD_D6P(AND_W_DN_EA, 16, PD_B_AND_DN_EA, PD_DISP_LO)
            PD_D6P(AND_L_DN_EA, 32, PD_B_AND_DN_EA, PD_DISP_LO)
            PD_D6P(OR_B_DN_EA, 8, PD_B_OR_DN_EA, PD_DISP_LO)
            PD_D6P(OR_W_DN_EA, 16, PD_B_OR_DN_EA, PD_DISP_LO)
            PD_D6P(OR_L_DN_EA, 32, PD_B_OR_DN_EA, PD_DISP_LO)
            PD_D6P(EOR_B_DN_EA, 8, PD_B_EOR_DN_EA, PD_DISP_LO)
            PD_D6P(EOR_W_DN_EA, 16, PD_B_EOR_DN_EA, PD_DISP_LO)
            PD_D6P(EOR_L_DN_EA, 32, PD_B_EOR_DN_EA, PD_DISP_LO)

            // ---- T0: #imm,ea ----
            PD_D6P_IMM(ADDI_B, 8, PD_B_ADDI)
            PD_D6P_IMM(ADDI_W, 16, PD_B_ADDI)
            PD_D6P_IMM(ADDI_L, 32, PD_B_ADDI)
            PD_D6P_IMM(SUBI_B, 8, PD_B_SUBI)
            PD_D6P_IMM(SUBI_W, 16, PD_B_SUBI)
            PD_D6P_IMM(SUBI_L, 32, PD_B_SUBI)
            PD_D6P_IMM(ANDI_B, 8, PD_B_ANDI)
            PD_D6P_IMM(ANDI_W, 16, PD_B_ANDI)
            PD_D6P_IMM(ANDI_L, 32, PD_B_ANDI)
            PD_D6P_IMM(ORI_B, 8, PD_B_ORI)
            PD_D6P_IMM(ORI_W, 16, PD_B_ORI)
            PD_D6P_IMM(ORI_L, 32, PD_B_ORI)
            PD_D6P_IMM(EORI_B, 8, PD_B_EORI)
            PD_D6P_IMM(EORI_W, 16, PD_B_EORI)
            PD_D6P_IMM(EORI_L, 32, PD_B_EORI)
            PD_D6P_IMM(CMPI_B, 8, PD_B_CMPI)
            PD_D6P_IMM(CMPI_W, 16, PD_B_CMPI)
            PD_D6P_IMM(CMPI_L, 32, PD_B_CMPI)

            // ---- T0: ADDQ/SUBQ, ADDA/SUBA/CMPA ----
            PD_D6P(ADDQ_B, 8, PD_B_ADDQ, PD_DISP_LO)
            PD_D6P(ADDQ_W, 16, PD_B_ADDQ, PD_DISP_LO)
            PD_D6P(ADDQ_L, 32, PD_B_ADDQ, PD_DISP_LO)
            PD_D6P(SUBQ_B, 8, PD_B_SUBQ, PD_DISP_LO)
            PD_D6P(SUBQ_W, 16, PD_B_SUBQ, PD_DISP_LO)
            PD_D6P(SUBQ_L, 32, PD_B_SUBQ, PD_DISP_LO)
            PD_CASE(PDF_ADDQ_AN, 0, 1, 32, PD_R(e.a) += e.b)
            PD_CASE(PDF_SUBQ_AN, 0, 1, 32, PD_R(e.a) -= e.b)
            PD_S7S(ADDA_W, 16, PD_B_ADDA)
            PD_S7S(ADDA_L, 32, PD_B_ADDA)
            PD_S7S(SUBA_W, 16, PD_B_SUBA)
            PD_S7S(SUBA_L, 32, PD_B_SUBA)
            PD_S7P(CMPA_W, 16, PD_B_CMPA)
            PD_S7P(CMPA_L, 32, PD_B_CMPA)

            // ---- T0: TST, CLR, unaries ----
            PD_S7P(TST_B, 8, PD_B_TST)
            PD_S7P(TST_W, 16, PD_B_TST)
            PD_S7P(TST_L, 32, PD_B_TST)
            PD_D6P(CLR_B, 8, PD_B_CLR, PD_DISP_LO)
            PD_D6P(CLR_W, 16, PD_B_CLR, PD_DISP_LO)
            PD_D6P(CLR_L, 32, PD_B_CLR, PD_DISP_LO)
            PD_PAIR(NEG_B_D, 8, PD_B_NEG)
            PD_PAIR(NEG_W_D, 16, PD_B_NEG)
            PD_PAIR(NEG_L_D, 32, PD_B_NEG)
            PD_PAIR(NOT_B_D, 8, PD_B_NOT)
            PD_PAIR(NOT_W_D, 16, PD_B_NOT)
            PD_PAIR(NOT_L_D, 32, PD_B_NOT)
            PD_PAIR(EXT_W, 16, PD_B_EXT_W)
            PD_PAIR(EXT_L, 32, PD_B_EXT_L)
            PD_PAIR(EXTB_L, 32, PD_B_EXTB_L)
            PD_PAIR(SWAP, 32, PD_B_SWAP)

            // ---- T0: shifts ----
            PD_SHIFT_PAIRS(B, 8)
            PD_SHIFT_PAIRS(W, 16)
            PD_SHIFT_PAIRS(L, 32)

            // ---- T0: multiply / divide ----
            PD_S7P(MULU_W, 16, PD_B_MULU)
            PD_S7P(MULS_W, 16, PD_B_MULS)
            PD_S7S(DIVU_W, 16, PD_B_DIVU)
            PD_S7S(DIVS_W, 16, PD_B_DIVS)

            // ---- T0: bit operations on Dn ----
            PD_CASE(PDF_BTST_I_D, 0, 2, 32, PD_B_BTST(32, PD_BIT_I, 1))
            PD_CASE(PDF_BTST_I_D + 1, 0, 2, 32, PD_B_BTST(32, PD_BIT_I, 0))
            PD_CASE(PDF_BCHG_I_D, 0, 2, 32, PD_B_BITOP(32, PD_BIT_I, _dst ^ _mask, 1))
            PD_CASE(PDF_BCHG_I_D + 1, 0, 2, 32, PD_B_BITOP(32, PD_BIT_I, _dst ^ _mask, 0))
            PD_CASE(PDF_BCLR_I_D, 0, 2, 32, PD_B_BITOP(32, PD_BIT_I, _dst & ~_mask, 1))
            PD_CASE(PDF_BCLR_I_D + 1, 0, 2, 32, PD_B_BITOP(32, PD_BIT_I, _dst & ~_mask, 0))
            PD_CASE(PDF_BSET_I_D, 0, 2, 32, PD_B_BITOP(32, PD_BIT_I, _dst | _mask, 1))
            PD_CASE(PDF_BSET_I_D + 1, 0, 2, 32, PD_B_BITOP(32, PD_BIT_I, _dst | _mask, 0))
            PD_CASE(PDF_BTST_D_D, 0, 1, 32, PD_B_BTST(32, PD_BIT_D, 1))
            PD_CASE(PDF_BTST_D_D + 1, 0, 1, 32, PD_B_BTST(32, PD_BIT_D, 0))
            PD_CASE(PDF_BCHG_D_D, 0, 1, 32, PD_B_BITOP(32, PD_BIT_D, _dst ^ _mask, 1))
            PD_CASE(PDF_BCHG_D_D + 1, 0, 1, 32, PD_B_BITOP(32, PD_BIT_D, _dst ^ _mask, 0))
            PD_CASE(PDF_BCLR_D_D, 0, 1, 32, PD_B_BITOP(32, PD_BIT_D, _dst & ~_mask, 1))
            PD_CASE(PDF_BCLR_D_D + 1, 0, 1, 32, PD_B_BITOP(32, PD_BIT_D, _dst & ~_mask, 0))
            PD_CASE(PDF_BSET_D_D, 0, 1, 32, PD_B_BITOP(32, PD_BIT_D, _dst | _mask, 1))
            PD_CASE(PDF_BSET_D_D + 1, 0, 1, 32, PD_B_BITOP(32, PD_BIT_D, _dst | _mask, 0))

            // ---- T0: CMPM, EXG, ABCD/SBCD ----
            PD_CASE(PDF_CMPM_B, 1, 1, 8, PD_B_CMPM(8, 1))
            PD_CASE(PDF_CMPM_B + 1, 1, 1, 8, PD_B_CMPM(8, PD_FLM))
            PD_CASE(PDF_CMPM_W, 1, 1, 16, PD_B_CMPM(16, 1))
            PD_CASE(PDF_CMPM_W + 1, 1, 1, 16, PD_B_CMPM(16, PD_FLM))
            PD_CASE(PDF_CMPM_L, 1, 1, 32, PD_B_CMPM(32, 1))
            PD_CASE(PDF_CMPM_L + 1, 1, 1, 32, PD_B_CMPM(32, PD_FLM))
            PD_CASE(PDF_EXG_DD, 0, 1, 32, uint32_t _t = PD_R(e.a); PD_R(e.a) = PD_R(e.b); PD_R(e.b) = _t)
            PD_CASE(PDF_EXG_AA, 0, 1, 32, uint32_t _t = PD_R(e.a); PD_R(e.a) = PD_R(e.b); PD_R(e.b) = _t)
            PD_CASE(PDF_EXG_DA, 0, 1, 32, uint32_t _t = PD_R(e.a); PD_R(e.a) = PD_R(e.b); PD_R(e.b) = _t)
            PD_CASE(PDF_ABCD_DD, 0, 1, 8, PD_STD8(e.a, abcd(cpu, (uint8_t)PD_R(e.a), (uint8_t)PD_R(e.b))))
            PD_CASE(PDF_SBCD_DD, 0, 1, 8, PD_STD8(e.a, sbcd(cpu, (uint8_t)PD_R(e.a), (uint8_t)PD_R(e.b))))

            // ---- T0: branches ----
            PD_BCC_ALL(B)
            PD_BCC_ALL(W)
        case PDF_BSR_B_IN:
        case PDF_BSR_W_IN: {
            PD_ENTER(e.a);
            PD_MAT(e.a);
            PD_PUSH32(cpu->pc); // the return address is the materialized PC
            PD_JUMP_IN(e.c);
        }
        case PDF_BSR_B_OUT:
        case PDF_BSR_W_OUT: {
            PD_ENTER(e.a);
            PD_MAT(e.a);
            PD_PUSH32(cpu->pc);
            PD_JUMP_PC(e.c);
        }
        case PDF_DBF_IN: {
            PD_ENTER(2);
            int16_t _counter = (int16_t)(PD_R(e.b) - 1);
            PD_STD16(e.b, _counter);
            if (_counter != -1)
                PD_JUMP_IN(e.c);
            PD_NEXT(2);
        }
        case PDF_DBF_OUT: {
            PD_ENTER(2);
            int16_t _counter = (int16_t)(PD_R(e.b) - 1);
            PD_STD16(e.b, _counter);
            if (_counter != -1)
                PD_JUMP_PC(e.c);
            PD_NEXT(2);
        }
        case PDF_DBCC_IN: {
            PD_ENTER(2);
            if (conditional_test(cpu, e.a))
                PD_NEXT(2);
            int16_t _counter = (int16_t)(PD_R(e.b) - 1);
            PD_STD16(e.b, _counter);
            if (_counter != -1)
                PD_JUMP_IN(e.c);
            PD_NEXT(2);
        }
        case PDF_DBCC_OUT: {
            PD_ENTER(2);
            if (conditional_test(cpu, e.a))
                PD_NEXT(2);
            int16_t _counter = (int16_t)(PD_R(e.b) - 1);
            PD_STD16(e.b, _counter);
            if (_counter != -1)
                PD_JUMP_PC(e.c);
            PD_NEXT(2);
        }
        case PDF_JMP_IND: {
            PD_ENTER(1);
            PD_JUMP_PC(PD_R(e.b));
        }
        case PDF_JMP_D16: {
            PD_ENTER(2);
            PD_JUMP_PC(PD_R(e.b) + (uint32_t)(int32_t)e.c);
        }
        case PDF_JMP_ABS: {
            PD_ENTER(e.b);
            PD_JUMP_PC(e.c);
        }
        case PDF_JSR_IND: {
            PD_ENTER(1);
            PD_MAT(1);
            uint32_t _ea = PD_R(e.b);
            PD_PUSH32(cpu->pc);
            PD_JUMP_PC(_ea);
        }
        case PDF_JSR_D16: {
            PD_ENTER(2);
            PD_MAT(2);
            uint32_t _ea = PD_R(e.b) + (uint32_t)(int32_t)e.c;
            PD_PUSH32(cpu->pc);
            PD_JUMP_PC(_ea);
        }
        case PDF_JSR_ABS: {
            PD_ENTER(e.b);
            PD_MAT(e.b);
            PD_PUSH32(cpu->pc);
            PD_JUMP_PC(e.c);
        }
        case PDF_RTS: {
            PD_ENTER(1);
            PD_MAT(1);
            uint32_t _pc = READ32(PD_SP);
            PD_SP += 4;
            PD_JUMP_PC(_pc);
        }
        case PDF_RTD: {
            PD_ENTER(2);
            PD_MAT(2);
            uint32_t _pc = READ32(PD_SP);
            PD_SP += 4;
            PD_SP += (uint32_t)(int32_t)e.c;
            PD_JUMP_PC(_pc);
        }
        case PDF_NOP: {
            PD_ENTER(1);
            PD_NEXT(1);
        }
        case PDF_UNLK: {
            PD_ENTER(1);
            PD_MAT(1);
            PD_SP = PD_R(e.a);
            uint32_t _v = READ32(PD_SP);
            PD_SP += 4;
            PD_R(e.a) = _v;
            PD_NEXT(1);
        }
        case PDF_LINK_W: {
            PD_ENTER(2);
            PD_MAT(2);
            uint32_t _ay = PD_R(e.a);
            PD_PUSH32(_ay);
            PD_R(e.a) = PD_SP;
            PD_SP += (uint32_t)(int32_t)e.c;
            PD_NEXT(2);
        }
        case PDF_LINK_L: {
            PD_ENTER(3);
            PD_MAT(3);
            uint32_t _a = PD_R(e.a);
            PD_PUSH32(_a);
            PD_R(e.a) = PD_SP;
            PD_SP += e.c;
            PD_NEXT(3);
        }
        case PDF_PEA_IND: {
            PD_ENTER(1);
            PD_MAT(1);
            PD_PUSH32(PD_R(e.b));
            PD_NEXT(1);
        }
        case PDF_PEA_D16: {
            PD_ENTER(2);
            PD_MAT(2);
            PD_PUSH32(PD_R(e.b) + (uint32_t)(int32_t)e.c);
            PD_NEXT(2);
        }
        case PDF_PEA_ABS: {
            PD_ENTER(e.b);
            PD_MAT(e.b);
            PD_PUSH32(e.c);
            PD_NEXT(e.b);
        }
        case PDF_LEA_IND: {
            PD_ENTER(1);
            PD_R(e.a) = PD_R(e.b);
            PD_NEXT(1);
        }
        case PDF_LEA_D16: {
            PD_ENTER(2);
            PD_R(e.a) = PD_R(e.b) + (uint32_t)(int32_t)e.c;
            PD_NEXT(2);
        }
        case PDF_LEA_ABS: {
            PD_ENTER(e.b);
            PD_R(e.a) = e.c;
            PD_NEXT(e.b);
        }

            // ---- T0: MOVEM ----
        case PDF_MOVEM_W_R_DEC: {
            PD_ENTER(2);
            PD_MAT(2);
            pd_movem_from_regs_predec(cpu, (uint16_t)e.c, e.a, 16);
            PD_NEXT(2);
        }
        case PDF_MOVEM_L_R_DEC: {
            PD_ENTER(2);
            PD_MAT(2);
            pd_movem_from_regs_predec(cpu, (uint16_t)e.c, e.a, 32);
            PD_NEXT(2);
        }
        case PDF_MOVEM_W_R_IND: {
            PD_ENTER(2);
            PD_MAT(2);
            pd_movem_from_regs(cpu, (uint16_t)e.c, PD_R(e.a), 16);
            PD_NEXT(2);
        }
        case PDF_MOVEM_L_R_IND: {
            PD_ENTER(2);
            PD_MAT(2);
            pd_movem_from_regs(cpu, (uint16_t)e.c, PD_R(e.a), 32);
            PD_NEXT(2);
        }
        case PDF_MOVEM_W_R_D16: {
            PD_ENTER(3);
            PD_MAT(3);
            pd_movem_from_regs(cpu, (uint16_t)e.c, PD_R(e.a) + (uint32_t)PD_DISP_HI, 16);
            PD_NEXT(3);
        }
        case PDF_MOVEM_L_R_D16: {
            PD_ENTER(3);
            PD_MAT(3);
            pd_movem_from_regs(cpu, (uint16_t)e.c, PD_R(e.a) + (uint32_t)PD_DISP_HI, 32);
            PD_NEXT(3);
        }
        case PDF_MOVEM_W_INC_R: {
            PD_ENTER(2);
            PD_MAT(2);
            uint32_t _ea = PD_R(e.a);
            PD_R(e.a) = _ea + 4; // calculate_ea(size 4) moves An first, as today
            pd_movem_to_regs(cpu, (uint16_t)e.c, _ea, 16, e.a);
            PD_NEXT(2);
        }
        case PDF_MOVEM_L_INC_R: {
            PD_ENTER(2);
            PD_MAT(2);
            uint32_t _ea = PD_R(e.a);
            PD_R(e.a) = _ea + 4;
            pd_movem_to_regs(cpu, (uint16_t)e.c, _ea, 32, e.a);
            PD_NEXT(2);
        }
        case PDF_MOVEM_W_IND_R: {
            PD_ENTER(2);
            PD_MAT(2);
            pd_movem_to_regs(cpu, (uint16_t)e.c, PD_R(e.a), 16, -1);
            PD_NEXT(2);
        }
        case PDF_MOVEM_L_IND_R: {
            PD_ENTER(2);
            PD_MAT(2);
            pd_movem_to_regs(cpu, (uint16_t)e.c, PD_R(e.a), 32, -1);
            PD_NEXT(2);
        }
        case PDF_MOVEM_W_D16_R: {
            PD_ENTER(3);
            PD_MAT(3);
            pd_movem_to_regs(cpu, (uint16_t)e.c, PD_R(e.a) + (uint32_t)PD_DISP_HI, 16, -1);
            PD_NEXT(3);
        }
        case PDF_MOVEM_L_D16_R: {
            PD_ENTER(3);
            PD_MAT(3);
            pd_movem_to_regs(cpu, (uint16_t)e.c, PD_R(e.a) + (uint32_t)PD_DISP_HI, 32, -1);
            PD_NEXT(3);
        }

            // ---- T0: Scc, traps ----
        case PDF_SCC_D: {
            PD_ENTER(1);
            PD_STD8(e.a, conditional_test(cpu, e.b) ? 0xFF : 0);
            PD_NEXT(1);
        }
        case PDF_ATRAP: {
            PD_ENTER(1);
            PD_MAT(1);
            EXC_ATRAP();
            goto relookup;
        }
        case PDF_TRAP: {
            PD_ENTER(1);
            PD_MAT(1);
            EXC_TRAP(e.b);
            goto relookup;
        }

        // ---- T1: every leaf of the decode tree, flattened ----
#include "cpu_pd_t1_cases.h"

        default:
            // An id the executor does not stamp (a family slot the classifier
            // never emits): treat as generic rather than trust it.
            (*instructions)++;
            cpu->pc = ipc;
            goto t2_step;
        }
    }
    // T1 leaves fall out of the switch with cpu->pc set by their body.
    goto relookup;

t2_step:
    // Generic tier: the switch core's per-instruction prologue, the one-
    // instruction executor, and a relookup from wherever the PC went.
    g_pd_stats.generic_steps++;
    {
#ifndef CPU_DECODER_IS_68030
        cpu->pc &= 0x00FFFFFFu; // 24-bit address bus (see cpu_68000.c)
        if (__builtin_expect(cpu->pc & 1u, 0)) { // odd PC: address error, delivered at `done`
            m68k_fetch_address_error(cpu);
            ipc = cpu->instruction_pc;
            goto done;
        }
#endif
        uint32_t fetch = memory_read_uint32(cpu->pc);
        uint16_t opcode = fetch >> 16;
        uint16_t ext_word = fetch & 0xFFFF;
        cpu->instruction_pc = cpu->pc;
        ipc = cpu->pc;
#ifndef CPU_DECODER_IS_68030
        if (__builtin_expect(!g_bus_error_pending, 1)) {
            cpu->ir = opcode;
            cpu->ir_pc = cpu->instruction_pc;
        }
#endif
        if (__builtin_expect(cpu->last_bus_error_pc != 0 && !cpu->supervisor && cpu->last_bus_error_pc != cpu->pc, 0))
            cpu->last_bus_error_pc = 0;
        cpu->pc += 2;
        if (*instructions > 0)
            (*instructions)--;
        PD_STEP_NAME(cpu, instructions, opcode, ext_word);
    }
    goto relookup;

relookup:
    // The only place a block pointer is derived from an address (§3.4).
    {
#ifdef CPU_DECODER_IS_68030
        uint32_t pc = cpu->pc;
#else
        uint32_t pc = cpu->pc & 0x00FFFFFFu;
#endif
        // The bus address: the inline accessors apply g_address_mask to
        // every access, and the fast-path tables are sized to it — a PC
        // above the map (an exception through a stray vector, a 24-bit
        // map under a 32-bit core) must not index past them.
        uint32_t bus = pc & g_address_mask;
        pd_slow = cpu->last_bus_error_pc != 0 && !cpu->supervisor;
        // Same logical page: reuse the block only if the page still maps to
        // the host page it was found on.  The mapping can change under a
        // logical address without leaving the page: an RTE or MOVE to SR
        // that drops to user mode swaps the active table (A/UX maps the
        // same user page differently in the two spaces), and a PMOVE /
        // PFLUSH through the generic step can remap it.
        if (blk && (pc - page_lo) < MEM_PAGE_SIZE && !(pc & 1u) &&
            g_active_read[bus >> PAGE_SHIFT] + (page_lo & g_address_mask) == (uintptr_t)blk->host) {
            cur = blk->e + ((pc - page_lo) >> 1);
            goto top; // same block, same mapping: no memory access
        }
        if (!blk && pd_held && ((pc ^ page_lo) & ~(uint32_t)PAGE_MASK) == 0)
            goto t2_loop; // still on a page the pool declined (demoted, held, no region)
        blk = NULL;
        pd_held = false;
        page_lo = pc & ~(uint32_t)PAGE_MASK;
        if (!(pc & 1u)) {
            uintptr_t base = g_active_read[bus >> PAGE_SHIFT];
            if (base != 0) {
                blk = predecode_lookup((uint8_t *)(base + (page_lo & g_address_mask)), page_lo, PD_ARCH_68K);
                if (!blk) {
                    g_pd_stats.relookup_nopool++;
                    pd_held = true;
                }
            } else {
                // No fast-path read entry yet (MMU page not walked, or a
                // device window): the generic step's fetch fills it, and
                // the next relookup finds the page — so no t2_loop shortcut.
                g_pd_stats.relookup_nomap++;
            }
        }
        predecode_enter(blk, *instructions); // charge the page being left, open the new one
        if (blk) {
            cur = blk->e + ((pc & PAGE_MASK) >> 1);
            goto top;
        }
        // Uncached (device window, logpointed, demoted) or an odd PC: the
        // generic tier until the page changes.
    }
t2_loop:
    if (*instructions == 0)
        goto done;
    goto t2_step;

done:
    // Materialize the architectural PC from the cursor (§3.4 step 7).
    if (blk) {
        uint32_t pc = page_lo + ((uint32_t)(cur - blk->e) << 1);
#ifdef CPU_DECODER_IS_68030
        cpu->pc = pc;
#else
        // The 68000 masks PC to its 24 address bits at each instruction's
        // prologue, so the register keeps a control transfer's full 32-bit
        // target until the next instruction runs (the switch core's
        // observable order).  When the sprint ends on that transfer the
        // cursor already agrees with the register modulo the mask: keep
        // the register, as the switch core would.
        if ((cpu->pc & 0x00FFFFFFu) != pc)
            cpu->pc = pc;
#endif
    }
    cpu->instruction_pc = ipc;
    predecode_enter(NULL, *instructions);
    // --- sprint exit: the switch core's epilogue ---
#ifdef CPU_DECODER_IS_68030
    if (__builtin_expect(g_bus_error_pending, 0)) {
        g_bus_error_pending = false;
#ifdef CPU_DECODER_IS_68040
        if (g_bus_error_is_pmmu)
#else
        if (g_mmu && g_mmu->enabled && g_bus_error_is_pmmu)
#endif
            exception_bus_error_retry(cpu, g_bus_error_address, g_bus_error_rw);
        else
            exception_bus_error(cpu, g_bus_error_address, g_bus_error_rw);
        g_active_read = cpu->supervisor ? g_supervisor_read : g_user_read;
        g_active_write = cpu->supervisor ? g_supervisor_write : g_user_write;
    } else if (__builtin_expect((_saved_trace & 2) && (cpu->trace & 2), 0)) {
        exception(cpu, 0x024, cpu->pc, cpu_get_sr(cpu));
    }
#else
    if (__builtin_expect(g_bus_error_pending, 0)) {
        g_bus_error_pending = false;
        if (!g_bus_error_is_address) // an address error keeps the PC the instruction reached
            cpu->pc = cpu->instruction_pc + 2;
        exception_bus_error(cpu, g_bus_error_address, g_bus_error_rw);
        g_active_read = cpu->supervisor ? g_supervisor_read : g_user_read;
        g_active_write = cpu->supervisor ? g_supervisor_write : g_user_write;
    }
#endif
    cpu_check_interrupt(cpu);
    assert(*instructions == 0);
}

// ============================================================================
// The classifier (rebinds every OP_ name: nothing below may execute ops).
// ============================================================================
#include "cpu_pd_classify.h"
