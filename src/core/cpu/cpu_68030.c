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
#include "fpu.h"
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
// Extension word bits [15:13] select the operation (per M68000PRM / MC68030UM):
//   000 = PMOVE TT0/TT1   (bits[12:10]: 010=TT0, 011=TT1)
//   001 = PLOAD / PFLUSH  (bits[12:10]: 000=PLOAD, 001=PFLUSHA, 100=PFLUSH FC#mask,
//                                       110=PFLUSH FC#mask+EA)
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
        // PMOVE TT0/TT1 (bits[12:10]: 010=TT0, 011=TT1)
        uint32_t reg_select = (ext >> 10) & 7u;
        uint32_t rw = (ext >> 9) & 1u; // 0=ea→reg, 1=reg→ea

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
        // PLOAD or PFLUSH (bits[12:10] = mode)
        uint32_t flush_mode = (ext >> 10) & 7u;
        if (flush_mode == 0) {
            // PLOAD — force a table walk and load the ATC entry
            uint32_t rw = (ext >> 9) & 1u; // 0=PLOADW (write test), 1=PLOADR (read test)
            if (mmu) {
                uint32_t ea = calculate_ea(cpu, 4, ea_mode, ea_reg, true);
                mmu_handle_fault(mmu, ea, rw == 0, cpu->supervisor != 0);
            }
        } else if (mmu) {
            // PFLUSH variants: invalidate ATC entries
            // mode=001: PFLUSHA (flush all entries)
            // mode=100: PFLUSH FC,#mask (flush by FC)
            // mode=110: PFLUSH FC,#mask,<ea> (flush by FC and EA)
            mmu_invalidate_tlb(mmu);
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
                    // PS field (bits 23:20) holds the page size exponent directly (valid: 8–15)
                    if (mmu->enabled) {
                        uint32_t sum = TC_IS(val) + TC_PS(val) + TC_TIA(val) + TC_TIB(val) + TC_TIC(val) + TC_TID(val);
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
        // Extension word bit 9 is the PTEST RW flag: per MC68030UM, 0 = PTESTW
        // (simulate a write access), 1 = PTESTR (simulate a read access).  Our
        // mmu_test_address takes `write` meaning "test as write", so map the
        // bit directly (rw==0 → write=true).
        uint32_t rw_bit = (ext >> 9) & 1u;
        bool write = (rw_bit == 0);
        uint32_t a_field = (ext >> 8) & 1u; // 1 = write descriptor addr to An
        uint32_t a_reg = (ext >> 5) & 7u;
        // FC specifier in extension word bits 4:0 (per MC68030UM § 7.4.30):
        //   1xxxx → immediate FC = bits 2:0
        //   01xxx → FC from data register Dn where n = bits 2:0
        //   00000 → FC from SFC register
        //   00001 → FC from DFC register
        // The aux kernel's soft-walk helper issues `PTESTW D0,(A0),#7,A0`
        // with D0 = user-data FC (1), and relies on PTEST walking the CRP
        // (user root) not the SRP (supervisor root).  Walking the wrong
        // root returns the supervisor 32-MB identity descriptor and makes
        // the shlib loader write libc1_s bytes to supervisor-mapped
        // phys (which is not where user pages actually live).
        uint32_t fc_spec = ext & 0x1Fu;
        uint32_t fc;
        if (fc_spec & 0x10u)
            fc = fc_spec & 7u; // immediate FC
        else if (fc_spec & 0x08u)
            fc = cpu->d[fc_spec & 7u] & 7u; // Dn low 3 bits
        else if (fc_spec == 0)
            fc = cpu->sfc;
        else if (fc_spec == 1)
            fc = cpu->dfc;
        else
            fc = cpu->supervisor != 0 ? 5u : 1u; // undefined encoding — fall back
        bool fc_supervisor = (fc & 4u) != 0; // FC bit 2 = supervisor
        uint32_t ea = calculate_ea(cpu, 4, ea_mode, ea_reg, true);
        if (mmu) {
            uint32_t desc_addr = 0;
            uint16_t status = mmu_test_address(mmu, ea, write, fc_supervisor, &desc_addr);
            mmu->mmusr = status;
            // A-reg output: per MC68030UM, when the A-reg field is specified,
            // the CPU writes the physical address of the last descriptor
            // examined during the walk into the selected A-register.  The aux
            // kernel's soft-walk helper uses `PTESTW ...,An` to read the PTE
            // it just tested — without this, the helper sees a stale An and
            // then reads nonsense memory, propagating an EFAULT up through
            // uiomove/readi into the shlib loader as errno=ENOEXEC.
            if (a_field)
                cpu->a[a_reg] = desc_addr;
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

// Hardware reset: emulates the RESET line being asserted by the GLU after a
// double bus error halt.  The sequence matches real hardware:
//   1. RESET asserts → peripherals reset (VIA re-enables ROM overlay at $0)
//   2. CPU reads SSP from $00000000 (ROM via overlay) and PC from $00000004
//   3. SR = $2700, VBR = 0, caches/MMU cleared
static __attribute__((noinline, cold)) void cpu_hardware_reset(cpu_t *restrict cpu) {
    LOG(1, "Hardware reset (double bus error → HALT → GLU RESET)");

    // Step 1: Assert RESET line → machine-specific peripheral reset.
    // On SE/30: VIA1 re-enables ROM overlay (ROM visible at $0), MMU disabled.
    // This MUST happen before the CPU reads vectors from $0.
    system_hardware_reset();

    // Step 2: CPU hardware reset sequence (MC68030 User's Manual §5.2.1)
    cpu->supervisor = 1;
    cpu->interrupt_mask = 7;
    cpu->trace = 0;
    cpu->vbr = 0;
    cpu->cacr = 0;
    cpu->ssp = memory_read_uint32(0x00000000); // SSP from vector 0 (ROM via overlay)
    cpu->a[7] = cpu->ssp;
    cpu->pc = memory_read_uint32(0x00000004); // PC from vector 1 (ROM via overlay)
}

// Generate the cpu_run_68030 decoder function using the shared template
#define CPU_DECODER_NAME        cpu_run_68030
#define CPU_DECODER_ARGS        cpu_t *restrict cpu, uint32_t *instructions
#define CPU_DECODER_RETURN_TYPE void
#define CPU_DECODER_PROLOGUE                                                                                           \
    /* Double bus error recovery: CPU halted → GLU asserts RESET line.                                               \
     * RESET resets peripherals (VIA overlay re-enabled: ROM at $0),                                                   \
     * then CPU loads SSP from $0 and PC from $4, SR=$2700, VBR=0. */                                                  \
    if (__builtin_expect(cpu->halted, 0)) {                                                                            \
        cpu->halted = 0;                                                                                               \
        cpu_hardware_reset(cpu);                                                                                       \
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
    /* Saturating decrement on the trailing (*instructions)--: memory_io_penalty                                       \
     * can clamp *instructions to 0 during the fetch (when the I/O penalty                                             \
     * equals or exceeds the remaining burndown), and an unconditional                                                 \
     * decrement would wrap to UINT32_MAX, breaking the                                                                \
     * sprint_burndown <= sprint_total invariant in scheduler.c:                                                       \
     * reconcile_sprint on any SE/30 sprint that ended its last instruction                                            \
     * on a slow I/O access. */                                                                                        \
    while (*instructions > 0) {                                                                                        \
        uint32_t fetch = memory_read_uint32(cpu->pc);                                                                  \
        uint16_t opcode = fetch >> 16;                                                                                 \
        uint16_t ext_word = fetch & 0xFFFF;                                                                            \
        cpu->instruction_pc = cpu->pc;                                                                                 \
        /* Double-fault tracking: a bus error on an instruction fetch leaves                                           \
         * last_bus_error_pc set so a retry at the SAME PC can be detected as                                          \
         * a true double fault.  The value must be cleared once the CPU has                                            \
         * moved past that PC in USER MODE — otherwise a different process                                           \
         * that later faults at the same VA (e.g. two execs of /etc/init,                                              \
         * both with crt0 at $148) is falsely flagged as a double fault.                                               \
         * Only clear in user mode: kernel-side instructions between the                                               \
         * first fault and the RTE retry must NOT clear the tracking, or                                               \
         * legitimate kernel-side double faults (and user retries that                                                 \
         * fault again at the same PC) would be missed. */                                                             \
        if (__builtin_expect(cpu->last_bus_error_pc != 0 && !cpu->supervisor && cpu->last_bus_error_pc != cpu->pc, 0)) \
            cpu->last_bus_error_pc = 0;                                                                                \
        cpu->pc += 2;                                                                                                  \
        if (*instructions > 0)                                                                                         \
            (*instructions)--;
#define CPU_DECODER_EPILOGUE                                                                                           \
    }                                                                                                                  \
    /* Trace exception: fire if T1 was set at sprint start AND still set now. */                                       \
    /* Checked outside the hot loop for zero per-instruction overhead. */                                              \
    /* For SR-modifying instructions, uses new T1 value (per M68000 PRM 6.3.10). */                                    \
    if (__builtin_expect((_saved_trace & 2) && (cpu->trace & 2), 0))                                                   \
        exception(cpu, 0x024, cpu->pc, cpu_get_sr(cpu));                                                               \
    /* Deferred bus error: handled outside the hot loop for zero overhead. */                                          \
    /* The memory slow path set *instructions=0 to force the loop exit. */                                             \
    if (__builtin_expect(g_bus_error_pending, 0)) {                                                                    \
        g_bus_error_pending = 0;                                                                                       \
        /* PMMU table-walk failures use Format $B (retry) so the kernel's                                              \
         * fault handler restarts the instruction after fixing the PTE.                                                \
         * Plain bus timeouts (unmapped physical in NuBus-probe range) use                                             \
         * Format $A (skip) so ROM probes advance past the bad access.                                                 \
         * g_bus_error_is_pmmu is set by mmu_handle_fault based on which                                               \
         * code path produced the false return. */                                                                     \
        if (g_mmu && g_mmu->enabled && g_bus_error_is_pmmu)                                                            \
            exception_bus_error_retry(cpu, g_bus_error_address, g_bus_error_rw);                                       \
        else                                                                                                           \
            exception_bus_error(cpu, g_bus_error_address, g_bus_error_rw);                                             \
        g_active_read = cpu->supervisor ? g_supervisor_read : g_user_read;                                             \
        g_active_write = cpu->supervisor ? g_supervisor_write : g_user_write;                                          \
    }                                                                                                                  \
    cpu_check_interrupt(cpu);                                                                                          \
    assert(*instructions == 0)

#include "cpu_decode.h"
