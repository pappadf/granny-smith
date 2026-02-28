// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_68030.c
// Motorola 68030 instruction decoder instantiation.
// Follows the same template pattern as cpu_68000.c: defines model-specific
// macros, includes cpu_ops.h (which #ifdef CPU_DECODER_IS_68030 overrides apply),
// then includes cpu_decode.h to generate cpu_run_68030().

#define CPU_MODEL_68030      68030
#define CPU_DECODER_IS_68030 1

#include "cpu_internal.h"
#include "mmu.h"

#include "log.h"
#include "system.h"
LOG_USE_CATEGORY_NAME("cpu");

// 68030 memory access: direct physical access (no MMU page table for now).
// These macros are identical to the 68000 path; MMU translation will be
// added in a later milestone when the full page table is wired up.
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

// ============================================================================
// 68030 MMU instruction dispatcher (PMOVE/PFLUSH/PTEST/PLOAD)
// ============================================================================
// Decodes the extension word to determine the MMU operation.
// Called from OP_PMMU_GENERAL (F-line CpID=0 type=0).
// Extension word bits [15:13] select the operation:
//   000 = PMOVE TT0/TT1, or PLOAD
//   001 = PFLUSH
//   010 = PMOVE (TC, SRP, CRP)
//   011 = PMOVE (MMUSR)
//   100 = PTEST

static void cpu_pmmu_general(cpu_t *cpu, uint16_t opcode) {
    uint16_t ext = fetch_16(cpu, true);
    mmu_state_t *mmu = (mmu_state_t *)cpu->mmu;
    uint32_t op_type = (ext >> 13) & 7u;

    // Compute EA from the opcode's lower 6 bits (modes that are valid for PMOVE)
    uint32_t ea_mode = (opcode >> 3) & 7u;
    uint32_t ea_reg = opcode & 7u;

    switch (op_type) {

    case 0: {
        // PMOVE TT0/TT1 or PLOAD
        uint32_t reg_select = (ext >> 10) & 7u;
        uint32_t rw = (ext >> 9) & 1u; // 0=ea→reg, 1=reg→ea

        if (reg_select == 0 || reg_select == 1) {
            // PLOAD (reg_select 0=PLOAD read, 1=PLOAD write)
            // Force a table walk without faulting
            if (mmu) {
                uint32_t ea = calculate_ea(cpu, 4, ea_mode, ea_reg, true);
                bool is_write = (reg_select == 1);
                mmu_handle_fault(mmu, ea, is_write, cpu->supervisor != 0);
            }
            break;
        }

        if (reg_select == 2) {
            // PMOVE TT0
            if (rw) {
                // TT0 → EA
                uint32_t ea = calculate_ea(cpu, 4, ea_mode, ea_reg, true);
                memory_write_uint32(ea, mmu ? mmu->tt0 : 0);
            } else {
                // EA → TT0
                uint32_t ea = calculate_ea(cpu, 4, ea_mode, ea_reg, true);
                uint32_t val = memory_read_uint32(ea);
                if (mmu) {
                    mmu->tt0 = val;
                    mmu_invalidate_tlb(mmu);
                }
            }
            break;
        }

        if (reg_select == 3) {
            // PMOVE TT1
            if (rw) {
                // TT1 → EA
                uint32_t ea = calculate_ea(cpu, 4, ea_mode, ea_reg, true);
                memory_write_uint32(ea, mmu ? mmu->tt1 : 0);
            } else {
                // EA → TT1
                uint32_t ea = calculate_ea(cpu, 4, ea_mode, ea_reg, true);
                uint32_t val = memory_read_uint32(ea);
                if (mmu) {
                    mmu->tt1 = val;
                    mmu_invalidate_tlb(mmu);
                }
            }
            break;
        }

        // Other reg_select values: undefined
        break;
    }

    case 1: {
        // PFLUSH
        uint32_t flush_mode = (ext >> 10) & 7u;
        if (mmu) {
            if (flush_mode == 1) {
                // PFLUSH by FC and EA — for now, invalidate everything
                mmu_invalidate_tlb(mmu);
            } else if (flush_mode == 4) {
                // PFLUSHA — flush all
                mmu_invalidate_tlb(mmu);
            } else {
                // Other modes: flush all as fallback
                mmu_invalidate_tlb(mmu);
            }
        }
        break;
    }

    case 2: {
        // PMOVE (TC, SRP, CRP)
        uint32_t reg_select = (ext >> 10) & 7u;
        uint32_t rw = (ext >> 9) & 1u; // 0=ea→reg, 1=reg→ea

        if (reg_select == 0) {
            // TC (Translation Control) — 32-bit
            if (rw) {
                // TC → EA
                uint32_t ea = calculate_ea(cpu, 4, ea_mode, ea_reg, true);
                memory_write_uint32(ea, mmu ? mmu->tc : 0);
            } else {
                // EA → TC
                uint32_t ea = calculate_ea(cpu, 4, ea_mode, ea_reg, true);
                uint32_t val = memory_read_uint32(ea);
                if (mmu) {
                    mmu->tc = val;
                    mmu->enabled = TC_ENABLE(val) != 0;
                    // Validate TC configuration: IS+PS+TIA+TIB+TIC+TID must equal 32
                    if (mmu->enabled) {
                        uint32_t sum =
                            TC_IS(val) + (TC_PS(val) + 8) + TC_TIA(val) + TC_TIB(val) + TC_TIC(val) + TC_TID(val);
                        if (sum != 32) {
                            // MMU configuration exception (vector 56 = 0xE0)
                            exception(cpu, 0xE0, cpu->pc, cpu_get_sr(cpu));
                            break;
                        }
                    }
                    mmu_invalidate_tlb(mmu);
                }
            }
            break;
        }

        if (reg_select == 2) {
            // SRP (Supervisor Root Pointer) — 64-bit
            if (rw) {
                // SRP → EA
                uint32_t ea = calculate_ea(cpu, 8, ea_mode, ea_reg, true);
                uint32_t upper = (uint32_t)(mmu ? mmu->srp >> 32 : 0);
                uint32_t lower = (uint32_t)(mmu ? mmu->srp & 0xFFFFFFFF : 0);
                memory_write_uint32(ea, upper);
                memory_write_uint32(ea + 4, lower);
            } else {
                // EA → SRP
                uint32_t ea = calculate_ea(cpu, 8, ea_mode, ea_reg, true);
                uint32_t upper = memory_read_uint32(ea);
                uint32_t lower = memory_read_uint32(ea + 4);
                if (mmu) {
                    uint64_t val = ((uint64_t)upper << 32) | lower;
                    // Validate DT field (bits 1:0 of upper word)
                    if ((upper & 3) == DESC_DT_INVALID) {
                        mmu->srp = val; // loaded before exception
                        exception(cpu, 0xE0, cpu->pc, cpu_get_sr(cpu));
                        break;
                    }
                    mmu->srp = val;
                    mmu_invalidate_tlb(mmu);
                }
            }
            break;
        }

        if (reg_select == 3) {
            // CRP (CPU Root Pointer) — 64-bit
            if (rw) {
                // CRP → EA
                uint32_t ea = calculate_ea(cpu, 8, ea_mode, ea_reg, true);
                uint32_t upper = (uint32_t)(mmu ? mmu->crp >> 32 : 0);
                uint32_t lower = (uint32_t)(mmu ? mmu->crp & 0xFFFFFFFF : 0);
                memory_write_uint32(ea, upper);
                memory_write_uint32(ea + 4, lower);
            } else {
                // EA → CRP
                uint32_t ea = calculate_ea(cpu, 8, ea_mode, ea_reg, true);
                uint32_t upper = memory_read_uint32(ea);
                uint32_t lower = memory_read_uint32(ea + 4);
                if (mmu) {
                    uint64_t val = ((uint64_t)upper << 32) | lower;
                    // Validate DT field (bits 1:0 of upper word)
                    if ((upper & 3) == DESC_DT_INVALID) {
                        mmu->crp = val; // loaded before exception
                        exception(cpu, 0xE0, cpu->pc, cpu_get_sr(cpu));
                        break;
                    }
                    mmu->crp = val;
                    mmu_invalidate_tlb(mmu);
                }
            }
            break;
        }

        // Other reg_select values: treat as no-op
        break;
    }

    case 3: {
        // PMOVE MMUSR — 16-bit
        uint32_t rw = (ext >> 9) & 1u;
        if (rw) {
            // MMUSR → EA (read)
            uint32_t ea = calculate_ea(cpu, 2, ea_mode, ea_reg, true);
            memory_write_uint16(ea, mmu ? mmu->mmusr : 0);
        } else {
            // EA → MMUSR (write) — some systems allow writing MMUSR
            uint32_t ea = calculate_ea(cpu, 2, ea_mode, ea_reg, true);
            uint16_t val = memory_read_uint16(ea);
            if (mmu)
                mmu->mmusr = val;
        }
        break;
    }

    case 4: {
        // PTEST
        uint32_t rw = (ext >> 9) & 1u; // 0=read test, 1=write test
        uint32_t ea = calculate_ea(cpu, 4, ea_mode, ea_reg, true);
        if (mmu) {
            uint16_t status = mmu_test_address(mmu, ea, rw != 0, cpu->supervisor != 0);
            mmu->mmusr = status;
            // If A-register field is specified (bits 8:5), store physical address
            // in that register (only for level > 0). Simplified: skip for now.
        }
        break;
    }

    default:
        // Reserved operation — ignore
        break;
    }
}

// ============================================================================

// MOVEC implementations — moved out of cpu_ops.h macros to reduce inline size.
// Called from OP_MOVEC_RC_RN / OP_MOVEC_RN_RC after the privilege check in SUPER().
// Returns 1 on success; calls illegal_instruction() and returns 0 on unknown control register.

static int cpu_movec_rc_rn(cpu_t *cpu) {
    uint16_t ext = fetch_16(cpu, true);
    uint32_t da = (ext >> 15) & 1u;
    uint32_t rn = (ext >> 12) & 7u;
    uint16_t rc = ext & 0x0FFFu;
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
    case 0x800:
        val = cpu->usp;
        break;
    case 0x801:
        val = cpu->vbr;
        break;
    case 0x802:
        val = cpu->caar;
        break;
    case 0x803:
        val = cpu->m ? cpu->a[7] : cpu->msp;
        break;
    case 0x804:
        val = cpu->m ? cpu->ssp : cpu->a[7];
        break;
    default:
        illegal_instruction(cpu);
        return 0;
    }
    if (da)
        cpu->a[rn] = val;
    else
        cpu->d[rn] = val;
    return 1;
}

static int cpu_movec_rn_rc(cpu_t *cpu) {
    uint16_t ext = fetch_16(cpu, true);
    uint32_t da = (ext >> 15) & 1u;
    uint32_t rn = (ext >> 12) & 7u;
    uint16_t rc = ext & 0x0FFFu;
    uint32_t val = da ? cpu->a[rn] : cpu->d[rn];
    switch (rc) {
    case 0x000:
        cpu->sfc = val & 7u;
        break;
    case 0x001:
        cpu->dfc = val & 7u;
        break;
    case 0x002:
        cpu->cacr = val & 0x3313u;
        break; /* 68030 CACR writable bits */
    case 0x800:
        cpu->usp = val;
        break;
    case 0x801:
        cpu->vbr = val;
        break;
    case 0x802:
        cpu->caar = val;
        break; /* cache address register: no-op */
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
    default:
        illegal_instruction(cpu);
        return 0;
    }
    return 1;
}

// Generate the cpu_run_68030 decoder function using the shared template
#define CPU_DECODER_NAME        cpu_run_68030
#define CPU_DECODER_ARGS        cpu_t *restrict cpu, uint32_t *instructions
#define CPU_DECODER_RETURN_TYPE void
#define CPU_DECODER_PROLOGUE                                                                                           \
    /* Set SoA active pointers based on current supervisor mode */                                                     \
    g_active_read = cpu->supervisor ? g_supervisor_read : g_user_read;                                                 \
    g_active_write = cpu->supervisor ? g_supervisor_write : g_user_write;                                              \
    cpu_check_interrupt(cpu);                                                                                          \
    while (*instructions > 0) {                                                                                        \
        uint32_t fetch = memory_read_uint32(cpu->pc);                                                                  \
        uint16_t opcode = fetch >> 16;                                                                                 \
        uint16_t ext_word = fetch & 0xFFFF;                                                                            \
        cpu->instruction_pc = cpu->pc;                                                                                 \
        cpu->pc += 2;                                                                                                  \
        uint32_t _saved_trace = cpu->trace;                                                                            \
        (*instructions)--;
#define CPU_DECODER_EPILOGUE                                                                                           \
    /* Trace exception: fire if T1 was set at instruction start AND still set now. */                                  \
    /* For SR-modifying instructions, uses new T1 value (per M68000 PRM 6.3.10). */                                    \
    /* For non-SR instructions, old==new since they don't change trace. */                                             \
    if ((_saved_trace & 2) && (cpu->trace & 2)) {                                                                      \
        exception(cpu, 0x024, cpu->pc, cpu_get_sr(cpu));                                                               \
    }                                                                                                                  \
    }                                                                                                                  \
    cpu_check_interrupt(cpu);                                                                                          \
    assert(*instructions == 0)

#include "cpu_decode.h"
