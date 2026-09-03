// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_68040.c
// Motorola 68040 instruction decoder instantiation.
// Follows the template pattern of cpu_68030.c: the 68040's integer unit is
// the 020/030 superset the shared cpu_ops.h already implements, so this file
// defines CPU_DECODER_IS_68030 to inherit that operation set, then overrides
// the handful of ops that differ on the 040 before generating the decoder:
//   - MOVE16 (all five forms) is a real instruction, not an F-line trap
//   - CINV/CPUSH decode + privilege are real (caches modeled functionally,
//     proposal §6.3: no line state to invalidate in an interpreter)
//   - PFLUSH/PTEST take their 040 forms (MMU registers live in mmu040_state_t,
//     reached via MOVEC — the 040 has no PMOVE)
//   - the 68851/030 coprocessor MMU ops (PMOVE dispatch, PBcc, PSAVE,
//     PRESTORE) are gone: they F-line trap
//   - MOVEC exposes the 040 control-register set (ITT/DTT/TC/URP/SRP/MMUSR)
//     and rejects the 030-only CAAR
//   - FSAVE/FRESTORE use the 040 single-longword frame formats

// CPU_MODEL_* are defined in cpu.h (included via cpu_internal.h).
#define CPU_DECODER_IS_68030 1
#define CPU_DECODER_IS_68040 1

#include "cpu_internal.h"
#include "fpu.h"
#include "mmu040.h"

#include "log.h"
#include "system.h"
LOG_USE_CATEGORY_NAME("cpu");

// 68040 memory access: identical to the 68030 path; the SoA fast path in
// memory.h resolves translations, and the (Phase B) 040 MMU fills it.
#define D(n)                                         cpu->d[n]
#define A(n)                                         cpu->a[n]
#define PC                                           cpu->pc
#define READ8(addr)                                  memory_read_uint8(addr)
#define READ16(addr)                                 memory_read_uint16(addr)
#define READ32(addr)                                 memory_read_uint32(addr)
#define WRITE8(addr, x)                              memory_write_uint8(addr, x)
#define WRITE16(addr, x)                             memory_write_uint16(addr, x)
#define WRITE32(addr, x)                             memory_write_uint32(addr, x)
#define FETCH8()                                     (uint8_t) fetch_16(cpu, true)
#define FETCH16()                                    fetch_16(cpu, true)
#define FETCH32()                                    fetch_32(cpu, true)
#define FETCH16_NO_INC()                             fetch_16(cpu, false)
#define FETCH32_NO_INC()                             fetch_32(cpu, false)
#define CC_C                                         cpu->carry
#define CC_X                                         cpu->extend
#define CC_N                                         cpu->negative
#define CC_V                                         cpu->overflow
#define CC_Z                                         cpu->zero
#define GET_USP()                                    (cpu->usp)
#define SET_USP(value_)                              (cpu->usp = (value_))
#define IS_SUPERVISOR()                              (cpu->supervisor != 0)
#define GET_SR()                                     cpu_get_sr(cpu)
#define SET_SR(value_)                               cpu_set_sr(cpu, (value_))
#define READ_CCR()                                   read_ccr(cpu)
#define WRITE_CCR(value_)                            write_ccr(cpu, (value_))
#define SBCD(dst, src)                               sbcd(cpu, (dst), (src))
#define ABCD(dst, src)                               abcd(cpu, (dst), (src))
#define MOVEM_FROM_REGISTER(op, sz)                  movem_from_register(cpu, (op), (sz))
#define MOVEM_TO_REGISTER(op, sz)                    movem_to_register(cpu, (op), (sz))
#define READ_EA(bits, opcode_, increment_)           read_ea_##bits(cpu, (opcode_), (increment_))
#define WRITE_EA(bits, mode_, reg_, value_)          write_ea_##bits(cpu, (mode_), (reg_), (value_))
#define CALCULATE_EA(size_, mode_, reg_, increment_) calculate_ea(cpu, (size_), (mode_), (reg_), (increment_))
#define CONDITIONAL_TEST(test_)                      conditional_test(cpu, (test_))
#define EXC_TRAP(vector_)                            trap(cpu, (vector_))
#define EXC_TRAPV()                                  trapv(cpu)
#define EXC_ATRAP()                                  a_trap(cpu)
#define EXC_FTRAP()                                  f_trap(cpu)
#define EXC_DIVIDE_BY_ZERO()                         exception_divide_by_zero(cpu)
#define EXC_CHK()                                    chk_exception(cpu)
#define EXC_PRIVILEGE()                              privilege_violation(cpu)
#define EXC_ILLEGAL()                                illegal_instruction(cpu)

#include "cpu_ops.h"

// Shorthand: the CPU-owned 040 MMU state (may be NULL in bare unit tests).
#define CPU_MMU040(cpu_) ((mmu040_state_t *)(cpu_)->mmu)

// ============================================================================
// 68040 MOVEC (control-register set per MC68040UM §3.1.2)
// ============================================================================
// Rc encodings: $000 SFC, $001 DFC, $002 CACR, $003 TC, $004 ITT0, $005 ITT1,
// $006 DTT0, $007 DTT1, $800 USP, $801 VBR, $803 MSP, $804 ISP, $805 MMUSR,
// $806 URP, $807 SRP.  CAAR ($802) is 020/030-only and takes an illegal-
// instruction exception on the 040.

// MOVEC Rc,Rn — returns 1 on success, 0 after raising illegal instruction.
static int cpu_movec040_rc_rn(cpu_t *cpu) {
    uint16_t ext = fetch_16(cpu, true);
    uint32_t da = (ext >> 15) & 1u;
    uint32_t rn = (ext >> 12) & 7u;
    uint16_t rc = ext & 0x0FFFu;
    mmu040_state_t *mmu = CPU_MMU040(cpu);
    uint32_t val = 0;
    switch (rc) {
    case 0x000:
        val = cpu->sfc;
        break;
    case 0x001:
        val = cpu->dfc;
        break;
    case 0x002:
        val = cpu->cacr;
        break;
    case 0x003:
        val = mmu ? mmu->tc : 0;
        break;
    case 0x004:
        val = mmu ? mmu->itt0 : 0;
        break;
    case 0x005:
        val = mmu ? mmu->itt1 : 0;
        break;
    case 0x006:
        val = mmu ? mmu->dtt0 : 0;
        break;
    case 0x007:
        val = mmu ? mmu->dtt1 : 0;
        break;
    case 0x800:
        val = cpu->usp;
        break;
    case 0x801:
        val = cpu->vbr;
        break;
    case 0x803:
        val = cpu->m ? cpu->a[7] : cpu->msp;
        break;
    case 0x804:
        val = cpu->m ? cpu->ssp : cpu->a[7];
        break;
    case 0x805:
        val = mmu ? mmu->mmusr : 0;
        break;
    case 0x806:
        val = mmu ? mmu->urp : 0;
        break;
    case 0x807:
        val = mmu ? mmu->srp : 0;
        break;
    default: // includes 030-only CAAR ($802)
        illegal_instruction(cpu);
        return 0;
    }
    if (da)
        cpu->a[rn] = val;
    else
        cpu->d[rn] = val;
    return 1;
}

// MOVEC Rn,Rc — returns 1 on success, 0 after raising illegal instruction.
static int cpu_movec040_rn_rc(cpu_t *cpu) {
    uint16_t ext = fetch_16(cpu, true);
    uint32_t da = (ext >> 15) & 1u;
    uint32_t rn = (ext >> 12) & 7u;
    uint16_t rc = ext & 0x0FFFu;
    mmu040_state_t *mmu = CPU_MMU040(cpu);
    uint32_t val = da ? cpu->a[rn] : cpu->d[rn];
    switch (rc) {
    case 0x000:
        cpu->sfc = val & 7u;
        break;
    case 0x001:
        cpu->dfc = val & 7u;
        break;
    case 0x002:
        // 68040 CACR: DE (bit 31) and IE (bit 15) are the only writable bits
        cpu->cacr = val & 0x80008000u;
        break;
    case 0x003:
        mmu040_set_tc(mmu, val);
        break;
    case 0x004:
        if (mmu)
            mmu040_set_ttr(mmu, &mmu->itt0, val);
        break;
    case 0x005:
        if (mmu)
            mmu040_set_ttr(mmu, &mmu->itt1, val);
        break;
    case 0x006:
        if (mmu)
            mmu040_set_ttr(mmu, &mmu->dtt0, val);
        break;
    case 0x007:
        if (mmu)
            mmu040_set_ttr(mmu, &mmu->dtt1, val);
        break;
    case 0x800:
        cpu->usp = val;
        break;
    case 0x801:
        cpu->vbr = val;
        break;
    case 0x803:
        if (cpu->m)
            cpu->a[7] = val;
        else
            cpu->msp = val;
        break;
    case 0x804:
        if (!cpu->m)
            cpu->a[7] = val;
        else
            cpu->ssp = val;
        break;
    case 0x805:
        if (mmu)
            mmu->mmusr = val;
        break;
    case 0x806:
        if (mmu)
            mmu040_set_root(mmu, &mmu->urp, val);
        break;
    case 0x807:
        if (mmu)
            mmu040_set_root(mmu, &mmu->srp, val);
        break;
    default: // includes 030-only CAAR ($802)
        illegal_instruction(cpu);
        return 0;
    }
    return 1;
}

// ============================================================================
// MOVE16 (M68000PRM: aligned 16-byte line copy)
// ============================================================================

// Copy one line; both addresses align down to a 16-byte boundary.  Returns
// false if any access faulted — the caller must then leave every address
// register at its pre-instruction value so the Format-$7 retry restarts
// clean (same restart-safety discipline as MOVEM/write_ea).
static bool cpu_move16_copy(uint32_t src, uint32_t dst) {
    src &= ~15u;
    dst &= ~15u;
    uint32_t line[4];
    for (int i = 0; i < 4; i++) {
        line[i] = memory_read_uint32(src + 4u * (uint32_t)i);
        if (__builtin_expect(g_bus_error_pending, 0))
            return false;
    }
    for (int i = 0; i < 4; i++) {
        memory_write_uint32(dst + 4u * (uint32_t)i, line[i]);
        if (__builtin_expect(g_bus_error_pending, 0))
            return false;
    }
    return true;
}

// ============================================================================
// CINV / CPUSH (MC68040UM §4: cache maintenance)
// ============================================================================
// Caches are modeled functionally (proposal §6.3): CACR holds the enable
// state, and CINV/CPUSH have correct decode and privilege, but there is no
// host-side line state to invalidate — the interpreter reads guest memory
// directly and the SoA arrays hold MMU translations (flushed by PFLUSH),
// not cached data.  DMA in this model is therefore always coherent — a
// documented divergence, more forgiving than hardware.
static void cpu_cache_op(cpu_t *cpu, uint16_t opcode) {
    (void)cpu;
    (void)opcode;
}

// ============================================================================
// 68040-specific operation overrides
// ============================================================================

// --- MOVE16: real line copies replacing the F-line stubs ---
#undef OP_MOVE16_AN_P_XXX_L
#define OP_MOVE16_AN_P_XXX_L                                                                                           \
    OP({                                                                                                               \
        uint32_t _abs = FETCH32();                                                                                     \
        if (cpu_move16_copy(A(EA_REG), _abs))                                                                          \
            A(EA_REG) += 16;                                                                                           \
    })
#undef OP_MOVE16_XXX_L_AN_P
#define OP_MOVE16_XXX_L_AN_P                                                                                           \
    OP({                                                                                                               \
        uint32_t _abs = FETCH32();                                                                                     \
        if (cpu_move16_copy(_abs, A(EA_REG)))                                                                          \
            A(EA_REG) += 16;                                                                                           \
    })
#undef OP_MOVE16_AN_XXX_L
#define OP_MOVE16_AN_XXX_L                                                                                             \
    OP({                                                                                                               \
        uint32_t _abs = FETCH32();                                                                                     \
        (void)cpu_move16_copy(A(EA_REG), _abs);                                                                        \
    })
#undef OP_MOVE16_XXX_L_AN
#define OP_MOVE16_XXX_L_AN                                                                                             \
    OP({                                                                                                               \
        uint32_t _abs = FETCH32();                                                                                     \
        (void)cpu_move16_copy(_abs, A(EA_REG));                                                                        \
    })
// (Ax)+,(Ay)+: destination register in bits 14:12 of the extension word.
// When Ax == Ay the register takes both increments (net +32), matching the
// PRM's two independent post-increments.
#undef OP_MOVE16_AN_P_AN_P
#define OP_MOVE16_AN_P_AN_P                                                                                            \
    OP({                                                                                                               \
        uint16_t _ext = FETCH16();                                                                                     \
        uint32_t _ay = (_ext >> 12) & 7u;                                                                              \
        if (cpu_move16_copy(A(EA_REG), A(_ay))) {                                                                      \
            A(EA_REG) += 16;                                                                                           \
            A(_ay) += 16;                                                                                              \
        }                                                                                                              \
    })

// --- CINV / CPUSH: real decode + privilege, functional effect ---
#undef OP_CINVL_CACHES_AN
#define OP_CINVL_CACHES_AN OP(SUPER(cpu_cache_op(cpu, opcode)))
#undef OP_CINVP_CACHES_AN
#define OP_CINVP_CACHES_AN OP(SUPER(cpu_cache_op(cpu, opcode)))
#undef OP_CINVA_CACHES
#define OP_CINVA_CACHES OP(SUPER(cpu_cache_op(cpu, opcode)))
#undef OP_CPUSHL_CACHES_AN
#define OP_CPUSHL_CACHES_AN OP(SUPER(cpu_cache_op(cpu, opcode)))
#undef OP_CPUSHP_CACHES_AN
#define OP_CPUSHP_CACHES_AN OP(SUPER(cpu_cache_op(cpu, opcode)))
#undef OP_CPUSHA_CACHES
#define OP_CPUSHA_CACHES OP(SUPER(cpu_cache_op(cpu, opcode)))

// --- PFLUSH (040 forms) ---
#undef OP_PFLUSHN_AN
#define OP_PFLUSHN_AN OP(SUPER(mmu040_pflush_page(CPU_MMU040(cpu), A(EA_REG), true)))
#undef OP_PFLUSH_AN
#define OP_PFLUSH_AN OP(SUPER(mmu040_pflush_page(CPU_MMU040(cpu), A(EA_REG), false)))
#undef OP_PFLUSHAN
#define OP_PFLUSHAN OP(SUPER(mmu040_pflush_all(CPU_MMU040(cpu), true)))
#undef OP_PFLUSHA
#define OP_PFLUSHA OP(SUPER(mmu040_pflush_all(CPU_MMU040(cpu), false)))

// --- PTEST (040 forms; address space selected by DFC per MC68040UM) ---
#undef OP_PTESTW_AN
#define OP_PTESTW_AN OP(SUPER(mmu040_ptest(CPU_MMU040(cpu), A(EA_REG), true, cpu->dfc)))
#undef OP_PTESTR_AN
#define OP_PTESTR_AN OP(SUPER(mmu040_ptest(CPU_MMU040(cpu), A(EA_REG), false, cpu->dfc)))

// --- 68851/030 coprocessor-interface MMU ops: gone on the 040 ---
#undef OP_PMMU_GENERAL
#define OP_PMMU_GENERAL OP(EXC_FTRAP())
#undef OP_PBCC_W
#define OP_PBCC_W OP(EXC_FTRAP())
#undef OP_PBCC_L
#define OP_PBCC_L OP(EXC_FTRAP())
#undef OP_PSAVE_EA
#define OP_PSAVE_EA OP(EXC_FTRAP())
#undef OP_PRESTORE_EA
#define OP_PRESTORE_EA OP(EXC_FTRAP())

// --- MOVEC: 040 control-register set ---
#undef OP_MOVEC_RC_RN
#define OP_MOVEC_RC_RN OP(SUPER(if (!cpu_movec040_rc_rn(cpu)) continue;))
#undef OP_MOVEC_RN_RC
#define OP_MOVEC_RN_RC OP(SUPER(if (!cpu_movec040_rn_rc(cpu)) continue;))

// --- FSAVE / FRESTORE: 040 single-longword frame formats ---
#undef OP_FSAVE_EA
#define OP_FSAVE_EA                                                                                                    \
    OP({                                                                                                               \
        if (!cpu->fpu) {                                                                                               \
            EXC_FTRAP();                                                                                               \
        } else                                                                                                         \
            SUPER({                                                                                                    \
                fpu_state_t *_fpu = (fpu_state_t *)cpu->fpu;                                                           \
                if (EA_MODE == 4) {                                                                                    \
                    AY -= 4; /* both NULL and IDLE frames are one longword */                                          \
                    fpu_fsave040(_fpu, AY);                                                                            \
                } else {                                                                                               \
                    uint32_t _ea = GET_EA;                                                                             \
                    fpu_fsave040(_fpu, _ea);                                                                           \
                }                                                                                                      \
            })                                                                                                         \
    })
#undef OP_FRESTORE_EA
#define OP_FRESTORE_EA                                                                                                 \
    OP({                                                                                                               \
        if (!cpu->fpu) {                                                                                               \
            EXC_FTRAP();                                                                                               \
        } else                                                                                                         \
            SUPER({                                                                                                    \
                fpu_state_t *_fpu = (fpu_state_t *)cpu->fpu;                                                           \
                uint32_t _ea = (EA_MODE == 3) ? AY : GET_EA;                                                           \
                int _sz = fpu_frestore040(_fpu, _ea);                                                                  \
                if (_sz < 0) {                                                                                         \
                    /* not this part's frame: format error (vector 14), PC at the FRESTORE */                          \
                    exception(cpu, 0x038, cpu->instruction_pc, GET_SR());                                              \
                } else if (EA_MODE == 3) {                                                                             \
                    AY += (uint32_t)_sz; /* (An)+ steps by the frame the format word declared */                       \
                }                                                                                                      \
            })                                                                                                         \
    })

// ============================================================================

// Hardware reset: same sequence as the 030 (see cpu_68030.c), plus the 040
// on-chip MMU/caches: TC.E cleared, TTRs disabled, CACR cleared.
static __attribute__((noinline, cold)) void cpu_hardware_reset_040(cpu_t *restrict cpu) {
    LOG(1, "Hardware reset (double bus error → HALT → RESET)");

    // Step 1: RESET line → machine-specific peripheral reset (overlay back on).
    system_hardware_reset();

    // Step 2: CPU hardware reset sequence (MC68040UM §7.1)
    cpu->supervisor = 1;
    cpu->interrupt_mask = 7;
    cpu->trace = 0;
    cpu->vbr = 0;
    cpu->cacr = 0;
    mmu040_state_t *mmu = CPU_MMU040(cpu);
    if (mmu) {
        mmu->tc = 0;
        mmu->itt0 = mmu->itt1 = mmu->dtt0 = mmu->dtt1 = 0;
        mmu->enabled = false;
        mmu040_invalidate_tlb(mmu);
    }
    cpu->ipl = 0;
    cpu->last_bus_error_pc = 0;
    g_bus_error_pending = false;
    cpu->ssp = memory_read_uint32(0x00000000); // SSP from vector 0 (ROM via overlay)
    cpu->a[7] = cpu->ssp;
    cpu->pc = memory_read_uint32(0x00000004); // PC from vector 1 (ROM via overlay)
}

// Generate the cpu_run_68040 decoder function using the shared template.
// Prologue/epilogue mirror cpu_68030.c; the model checks inside the shared
// exception helpers select the 040 frame formats (Format $7 access error).
#define CPU_DECODER_NAME        cpu_run_68040_switch
#define CPU_DECODER_ARGS        cpu_t *restrict cpu, uint32_t *instructions
#define CPU_DECODER_RETURN_TYPE void
#define CPU_DECODER_PROLOGUE                                                                                           \
    /* Double bus error recovery: CPU halted → RESET (see cpu_68030.c). */                                           \
    if (__builtin_expect(cpu->halted, 0)) {                                                                            \
        cpu->halted = 0;                                                                                               \
        cpu_hardware_reset_040(cpu);                                                                                   \
    }                                                                                                                  \
    /* Set SoA active pointers based on current supervisor mode */                                                     \
    g_active_read = cpu->supervisor ? g_supervisor_read : g_user_read;                                                 \
    g_active_write = cpu->supervisor ? g_supervisor_write : g_user_write;                                              \
    cpu_check_interrupt(cpu);                                                                                          \
    g_bus_error_instr_ptr = instructions; /* let memory slow paths force exit */                                       \
    /* Capture trace state before execution; clamp to 1 instruction if T1 set */                                       \
    uint32_t _saved_trace = cpu->trace;                                                                                \
    if (__builtin_expect(_saved_trace & 2, 0))                                                                         \
        if (*instructions > 1)                                                                                         \
            *instructions = 1;                                                                                         \
    /* Saturating decrement on the trailing (*instructions)--: see cpu_68030.c */                                      \
    while (*instructions > 0) {                                                                                        \
        uint32_t fetch = memory_read_uint32(cpu->pc);                                                                  \
        uint16_t opcode = fetch >> 16;                                                                                 \
        uint16_t ext_word = fetch & 0xFFFF;                                                                            \
        cpu->instruction_pc = cpu->pc;                                                                                 \
        /* Double-fault tracking: see the cpu_68030.c prologue for why this  */                                        \
        /* clears only in user mode once the CPU has moved past the PC.     */                                         \
        if (__builtin_expect(cpu->last_bus_error_pc != 0 && !cpu->supervisor && cpu->last_bus_error_pc != cpu->pc, 0)) \
            cpu->last_bus_error_pc = 0;                                                                                \
        cpu->pc += 2;                                                                                                  \
        if (*instructions > 0)                                                                                         \
            (*instructions)--;
#define CPU_DECODER_EPILOGUE                                                                                           \
    }                                                                                                                  \
    /* Deferred bus error: delivered as a Format $7 access error.  MMU     */                                          \
    /* descriptor faults use retry semantics (handler maps the page, RTE   */                                          \
    /* restarts the instruction); plain bus timeouts use skip semantics so */                                          \
    /* ROM probes advance past the bad access — same split as the 030.     */                                        \
    if (__builtin_expect(g_bus_error_pending, 0)) {                                                                    \
        g_bus_error_pending = false;                                                                                   \
        if (g_bus_error_is_pmmu)                                                                                       \
            exception_bus_error_retry(cpu, g_bus_error_address, g_bus_error_rw);                                       \
        else                                                                                                           \
            exception_bus_error(cpu, g_bus_error_address, g_bus_error_rw);                                             \
        g_active_read = cpu->supervisor ? g_supervisor_read : g_user_read;                                             \
        g_active_write = cpu->supervisor ? g_supervisor_write : g_user_write;                                          \
    } else if (__builtin_expect((_saved_trace & 2) && (cpu->trace & 2), 0)) {                                          \
        /* Trace exception: fire if T1 was set at sprint start AND still set now. */                                   \
        exception(cpu, 0x024, cpu->pc, cpu_get_sr(cpu));                                                               \
    }                                                                                                                  \
    cpu_check_interrupt(cpu);                                                                                          \
    assert(*instructions == 0)

#include "cpu_decode.h"
#undef CPU_DECODER_NAME
#undef CPU_DECODER_ARGS
#undef CPU_DECODER_RETURN_TYPE
#undef CPU_DECODER_PROLOGUE
#undef CPU_DECODER_EPILOGUE

// ============================================================================
// The predecoded executor (proposal-predecoded-interpreter-cores.md): the
// one-instruction executor, the sprint loop over predecoded entries, and
// the decode tree in its classifier role — three more instantiations of
// the same template, sharing this file's macro bindings and op bodies.
// ============================================================================
#define PD_RUN_NAME      cpu_pd_run_68040
#define PD_STEP_NAME     cpu_pd_step_68040
#define PD_DECODE_NAME   cpu_pd_decode_68040
#define PD_TREE_NAME     cpu_pd_tree_68040
#define PD_CLASSIFY_NAME cpu_pd_classify_68040
#define PD_HW_RESET(c)   cpu_hardware_reset_040(c)
#include "cpu_pd_run.h"

// The core's entry point: the predecoded executor when enabled, else the
// switch core (kept for A/B from the shell: machine.cpu.predecode = 0).
void cpu_run_68040(cpu_t *restrict cpu, uint32_t *instructions) {
    if (predecode_enabled())
        cpu_pd_run_68040(cpu, instructions);
    else
        cpu_run_68040_switch(cpu, instructions);
}
