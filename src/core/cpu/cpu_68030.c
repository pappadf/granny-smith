// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_68030.c
// Motorola 68030 instruction decoder instantiation.
// Follows the same template pattern as cpu_68000.c: takes the shared macro
// set, includes cpu_ops.h (which #ifdef CPU_DECODER_IS_68030 overrides apply),
// then includes cpu_decode.h to generate cpu_run_68030().

// CPU_MODEL_68030 is defined in cpu.h (included via cpu_internal.h).
#define CPU_DECODER_IS_68030 1

#include "cpu_internal.h"
#include "cpu_movec.h"
#include "fpu.h"
#include "gs_assert.h"
#include "mmu.h"
#include "pmmu_ext.h"

#include "log.h"
#include "system.h"
LOG_USE_CATEGORY_NAME("cpu");

// Operand/memory/flag/exception macros shared by all three 68K decoders
#include "cpu_decoder_macros.h"
#include "cpu_sprint.h"

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

// The function code a PLOAD or PTEST names in extension bits 4:0
// (MC68030UM 7.4.27, 7.4.30): 1xxxx an immediate FC in bits 2:0, 01xxx the
// low three bits of Dn, 00000 SFC, 00001 DFC.
static uint32_t pmmu_fc(cpu_t *cpu, uint32_t fc_spec) {
    if (fc_spec & 0x10u)
        return fc_spec & 7u; // immediate FC
    if (fc_spec & 0x08u)
        return cpu->d[fc_spec & 7u] & 7u; // Dn low 3 bits
    if (fc_spec == 0)
        return cpu->sfc;
    if (fc_spec == 1)
        return cpu->dfc;
    return cpu->supervisor != 0 ? 5u : 1u; // undefined encoding -- fall back
}

// PMOVE of a 32-bit transparent-translation register: to <ea> (rw = 1) or
// from it, then flush the ATC.  TT ignores FD: over-flushing is
// architecturally invisible (MC68030UM 9.4).  field is NULL with no MMU.
static void pmove_tt(cpu_t *cpu, mmu_state_t *mmu, uint32_t ea_mode, uint32_t ea_reg, uint32_t rw, uint32_t *field) {
    uint32_t ea = calculate_ea(cpu, 4, ea_mode, ea_reg, true);
    if (rw) {
        memory_write_uint32(ea, field ? *field : 0);
        return;
    }
    uint32_t val = memory_read_uint32(ea);
    if (g_bus_error_pending)
        return; // a faulted operand read commits nothing
    if (field) {
        *field = val;
        mmu_invalidate_tlb(mmu);
    }
}

// PMOVE of a 64-bit root pointer (SRP or CRP): to <ea> (rw = 1) or from it.
// root is NULL with no MMU; fd (Force Descriptor) suppresses the ATC flush.
static void pmove_root(cpu_t *cpu, mmu_state_t *mmu, uint32_t ea_mode, uint32_t ea_reg, uint32_t rw, uint32_t fd,
                       uint64_t *root) {
    uint32_t ea = calculate_ea(cpu, 8, ea_mode, ea_reg, true);
    if (rw) {
        uint64_t val = root ? *root : 0;
        memory_write_uint32(ea, (uint32_t)(val >> 32));
        memory_write_uint32(ea + 4, (uint32_t)val);
        return;
    }
    // A faulting operand fetch must not commit a half-read root.
    // Every comparable multi-access op guards; cpu_pmmu_general did
    // not, so a bus error on either long left the register loaded
    // from whatever the failed read returned.
    uint32_t upper = memory_read_uint32(ea);
    if (g_bus_error_pending)
        return;
    uint32_t lower = memory_read_uint32(ea + 4);
    if (g_bus_error_pending)
        return;
    if (!root)
        return;
    *root = ((uint64_t)upper << 32) | lower;
    // Validate DT field (bits 1:0 of upper word)
    if ((upper & 3) == DESC_DT_INVALID) {
        // MC68030UM 9.7.5.3: the register is loaded before the
        // exception is taken (commit-then-except -- this is a
        // post-instruction exception, so validate-before-commit
        // would be wrong).  The flush was being skipped by the
        // early break, leaving ATC entries from the old root.
        mmu_invalidate_tlb(mmu);
        exception(cpu, M68K_VEC_MMU_CONFIG, cpu->pc, cpu_get_sr(cpu));
        return;
    }
    if (!fd)
        mmu_invalidate_tlb(mmu);
}

static void cpu_pmmu_general(cpu_t *cpu, uint16_t opcode) {
    uint16_t ext = fetch_16(cpu, true);
    mmu_state_t *mmu = (mmu_state_t *)cpu->mmu;
    pmmu_ext_t x = pmmu_ext_decode(ext); // the fields, decoded once (shared with the disassembler)

    // Compute EA from the opcode's lower 6 bits (modes that are valid for PMOVE)
    uint32_t ea_mode = (opcode >> 3) & 7u;
    uint32_t ea_reg = opcode & 7u;

    switch (x.op) {

    case PMMU_OP_PMOVE_TT: {
        // PMOVE TT0/TT1 (bits[12:10]: 010=TT0, 011=TT1)
        uint32_t reg_select = x.sel;
        uint32_t rw = x.rw; // 0=ea→reg, 1=reg→ea

        if (reg_select == 2 || reg_select == 3) {
            // PMOVE TT0 / TT1
            uint32_t *tt = !mmu ? NULL : reg_select == 2 ? &mmu->tt0 : &mmu->tt1;
            pmove_tt(cpu, mmu, ea_mode, ea_reg, rw, tt);
            break;
        }

        // Other reg_select values: undefined
        break;
    }

    case PMMU_OP_PLOAD_PFLUSH: {
        // PLOAD or PFLUSH (bits[12:10] = mode)
        uint32_t flush_mode = x.sel;
        if (flush_mode == 0) {
            // PLOAD — force a table walk and load the ATC entry
            uint32_t rw = x.rw; // 0=PLOADW (write test), 1=PLOADR (read test)
            if (mmu) {
                // FC specifier in extension word bits 4:0 (per MC68030UM § 7.4.27),
                // same encoding as PTEST: 1xxxx → immediate FC = bits 2:0,
                // 01xxx → FC from data register Dn, 00000 → SFC, 00001 → DFC.
                // Walking the wrong root (always cpu->supervisor) would load the
                // ATC with a translation from the supervisor-side root for a
                // user-FC PLOAD.
                uint32_t fc = pmmu_fc(cpu, x.fc5);
                bool fc_supervisor = (fc & 4u) != 0;
                uint32_t ea = calculate_ea(cpu, 4, ea_mode, ea_reg, true);
                mmu_pload(mmu, ea, rw == 0, fc_supervisor);
            }
        } else if (flush_mode == 1u || flush_mode == 4u || flush_mode == 6u) {
            // The only three PFLUSH modes the MC68030 implements (M68000PRM
            // PFLUSH, "Mode field"): 001 PFLUSHA, 100 PFLUSH FC,#mask,
            // 110 PFLUSH FC,#mask,<ea>.  All three take the full flush --
            // over-flushing is architecturally invisible, because MC68030UM
            // 9.4 lets the replacement algorithm discard any valid entry at
            // any time, so a guest cannot tell a selective flush from a total
            // one.
            if (mmu)
                mmu_invalidate_tlb(mmu);
        } else {
            // Modes 010/011/101/111 are not MC68030 encodings; 010/011 are the
            // 68851's PFLUSHS.  MC68030UM 10.3 lists PFLUSHS among the
            // instructions that "must be avoided or emulated in the exception
            // routine for F-line unimplemented instructions".  Flushing the
            // whole ATC instead hid them completely.
            f_trap(cpu, opcode);
            return;
        }
        break;
    }

    case PMMU_OP_PMOVE_ROOT: {
        // PMOVE (TC, SRP, CRP)
        uint32_t reg_select = x.sel;
        uint32_t rw = x.rw; // 0=ea→reg, 1=reg→ea

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
                if (g_bus_error_pending)
                    break;
                // FD (Force Descriptor) bit 8: when set, suppress ATC flush.
                // Used by ROM "swap MMU state" sequences that need the OLD
                // mapping to remain valid for the immediately following
                // operand fetch (e.g. IIfx $40804C02 PMOVEFD CRP / $40804C06
                // PMOVE TC — the TC operand at $8(A0) lives in the OLD
                // mapping and a premature ATC flush in PMOVEFD CRP makes it
                // unreachable, faulting the TC load and causing the boot's
                // reset loop).
                uint32_t fd = x.fd;
                if (mmu) {
                    // CACR bit 0 (EI): the instruction cache keeps serving the
                    // code around the PC across the switch (mmu.h).
                    if ((cpu->cacr & 1u) && !mmu->enabled && TC_ENABLE(val))
                        mmu_icache_hold(mmu, cpu->pc, cpu->supervisor);
                    mmu->tc = val;
                    mmu->enabled = TC_ENABLE(val) != 0;
                    // Validate TC configuration: IS+PS+TIA+TIB+TIC+TID must equal 32
                    // PS field (bits 23:20) holds the page size exponent directly (valid: 8–15)
                    if (mmu->enabled) {
                        uint32_t sum = TC_IS(val) + TC_PS(val) + TC_TIA(val) + TC_TIB(val) + TC_TIC(val) + TC_TID(val);
                        // MC68030UM 9.7.5.3: the consistency check fails if the
                        // sum is not 32, AND separately if PS holds one of the
                        // reserved values $0-$7.  Only the sum was checked.
                        if (sum != 32 || TC_PS(val) < 8) {
                            // MC68030UM 9.7.2: "If an MMU configuration
                            // exception occurs, the TC register is updated with
                            // the data, and the E bit is cleared."  Leaving
                            // enabled set returned a guest probing for a
                            // supported configuration from vector 56 with
                            // translation still ON.  The flush must happen too:
                            // the early break used to skip it, stranding ATC
                            // entries built under the old TC.
                            mmu->enabled = false;
                            mmu_invalidate_tlb(mmu);
                            exception(cpu, M68K_VEC_MMU_CONFIG, cpu->pc, cpu_get_sr(cpu));
                            break;
                        }
                    }
                    if (!fd)
                        mmu_invalidate_tlb(mmu);
                }
            }
            break;
        }

        if (reg_select == 2 || reg_select == 3) {
            // SRP (Supervisor Root Pointer) / CRP (CPU Root Pointer) -- 64-bit
            uint32_t fd = x.fd; // FD: suppress ATC flush (see TC case above)
            uint64_t *root = !mmu ? NULL : reg_select == 2 ? &mmu->srp : &mmu->crp;
            pmove_root(cpu, mmu, ea_mode, ea_reg, rw, fd, root);
            break;
        }

        // Other reg_select values: treat as no-op
        break;
    }

    case PMMU_OP_PMOVE_MMUSR: {
        // PMOVE MMUSR — 16-bit
        uint32_t rw = x.rw;
        if (rw) {
            // MMUSR → EA (read)
            uint32_t ea = calculate_ea(cpu, 2, ea_mode, ea_reg, true);
            memory_write_uint16(ea, mmu ? mmu->mmusr : 0);
        } else {
            // EA → MMUSR (write) — some systems allow writing MMUSR
            uint32_t ea = calculate_ea(cpu, 2, ea_mode, ea_reg, true);
            uint16_t val = memory_read_uint16(ea);
            if (g_bus_error_pending)
                break; // a faulted operand read commits nothing
            if (mmu)
                mmu->mmusr = val;
        }
        break;
    }

    case PMMU_OP_PTEST: {
        // PTEST
        // Extension word bit 9 is the PTEST RW flag: per MC68030UM, 0 = PTESTW
        // (simulate a write access), 1 = PTESTR (simulate a read access).  Our
        // mmu_test_address takes `write` meaning "test as write", so map the
        // bit directly (rw==0 → write=true).
        uint32_t rw_bit = x.rw;
        bool write = (rw_bit == 0);
        uint32_t a_field = x.fd; // 1 = write descriptor addr to An
        uint32_t a_reg = x.mask;
        uint32_t level = x.sel;
        // M68000PRM PTEST, "Level field": "When this field contains 0, the A
        // field and the register field must also be 0.  The instruction takes
        // an F-line exception when the level field is 0 and the A field is not
        // 0."  A level-0 PTEST searches only the ATC, so there is no "last
        // descriptor searched" to hand back, which is why the encoding is
        // illegal rather than merely useless.
        if (level == 0 && a_field != 0) {
            f_trap(cpu, opcode);
            return;
        }
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
        uint32_t fc = pmmu_fc(cpu, x.fc5);
        bool fc_supervisor = (fc & 4u) != 0; // FC bit 2 = supervisor
        uint32_t ea = calculate_ea(cpu, 4, ea_mode, ea_reg, true);
        if (mmu) {
            uint32_t desc_addr = 0;
            uint16_t status = mmu_ptest(mmu, ea, write, fc_supervisor, level, &desc_addr);
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
    movec_ext_t x = movec_ext(cpu);
    uint32_t val = 0;
    if (!movec_common_read(cpu, x.rc, &val)) {
        switch (x.rc) {
        case 0x002:
            val = cpu->cacr;
            break;
        case 0x802:
            val = cpu->caar;
            break;
        default:
            illegal_instruction(cpu);
            return 0;
        }
    }
    *x.rn = val;
    return 1;
}

static int cpu_movec_rn_rc(cpu_t *cpu) {
    movec_ext_t x = movec_ext(cpu);
    uint32_t val = *x.rn;
    if (movec_common_write(cpu, x.rc, val))
        return 1;
    switch (x.rc) {
    case 0x002:
        cpu->cacr = val & 0x3313u;
        break; /* 68030 CACR writable bits */
    case 0x802:
        cpu->caar = val;
        break; /* cache address register: no-op */
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
// The CPU half of a reset, and only that half: everything inside the package.
// MC68030 User's Manual §5.2.1.  Exported because level 2 -- machine.reset(),
// the reset button, Cuda CMD_RESET -- is "bus_reset plus this", and the Cuda
// path used to do neither on a 68k machine.
//
// The caller must have asserted the bus reset FIRST: the vectors are read from
// $00000000, which is ROM only while the overlay is armed, and re-arming the
// overlay is the bus half's job.
void cpu_reset_to_vector_68030(cpu_t *restrict cpu) {
    // CPU hardware reset sequence (MC68030 User's Manual §5.2.1)
    cpu->supervisor = 1;
    // The access pair follows the mode.  The 68030 decoder re-picks it every
    // sprint, but the 68000 (which shares this reset) only on a mode change,
    // so a reset taken in user mode kept the user pair: the Lisa boot ROM
    // then ran its MMU tests through the user context and bus-faulted on its
    // own I/O strobes once it selected context 1 (boot error 40).
    cpu_select_soa(true);
    cpu->interrupt_mask = 7;
    cpu->trace = 0;
    cpu->vbr = 0;
    cpu->cacr = 0;
    // Clear pre-halt latches so the reset doesn't inherit the state that
    // caused the double-bus-error halt in the first place.  Clearing `halted`
    // here is the other half of MC68030UM 7.5.4's rule -- "Only an external
    // reset operation can restart a halted processor" -- and it is what lets
    // the 68000 simply STAY halted rather than resuming on its own.
    cpu->halted = 0;
    // A reset also ends a STOP: the core resumes only on an interrupt, and the
    // mask is 7 from here on, so a CPU left stopped would sleep forever.
    cpu->stopped = 0;
    cpu->ipl = 0;
    cpu->nmi_edge = 0;
    cpu->last_bus_error_pc = 0;
    g_bus_error_pending = false;
    cpu->ssp = memory_read_uint32(0x00000000); // SSP from vector 0 (ROM via overlay)
    cpu->a[7] = cpu->ssp;
    cpu->pc = memory_read_uint32(0x00000004); // PC from vector 1 (ROM via overlay)

    // The PMMU is INSIDE the 68030, so its reset belongs here and not on the
    // board's /RESET net.  The family bus_reset handlers used to clear it --
    // the 68k board reset took an `mmu` argument for exactly this -- which put
    // CPU-internal state on the wrong side of the package boundary, the only
    // line the hardware actually draws.  The 68040 equivalent was already on
    // this side, in cpu_hardware_reset_040.
    //
    // MC68030 UM 3rd ed. §9.2.2: "When the MC68030 is reset by the assertion
    // of the RESET signal, the E bits of the TC and TTx registers are
    // cleared, disabling address translation."  TT0/TT1 used to survive: a
    // guest that had set up transparent translation (System 7 in 32-bit mode
    // on a IIci) kept it across a reset, so the ROM's next pass ran with I/O
    // windows mapped that a cold machine does not have, jumped through a word
    // of RAM-test fill ($AAAAAAAA) and double-faulted into a reset loop.
    mmu_state_t *mmu = (mmu_state_t *)cpu->mmu;
    if (mmu) {
        mmu->enabled = false;
        mmu->tc = 0;
        mmu->tt0 &= ~0x8000u; // TTx.E, bit 15
        // ...except where the board holds TT1 at a value of its own from
        // power-on: a reset brings back the machine as it powers on.
        mmu->tt1 = mmu->tt1_board ? mmu->tt1_board : (mmu->tt1 & ~0x8000u);
        mmu_invalidate_tlb(mmu);
    }
}

// Generate the cpu_run_68030 decoder function using the shared template
#define CPU_DECODER_NAME        cpu_run_68030_switch
#define CPU_DECODER_ARGS        cpu_t *restrict cpu, uint32_t *instructions
#define CPU_DECODER_RETURN_TYPE void
#define CPU_DECODER_PROLOGUE                                                                                           \
    /* Sprint entry (cpu_sprint.h): reset after a double bus fault, T1 sampling */                                     \
    uint32_t saved_trace = m68k_sprint_enter(cpu, instructions);                                                       \
    /* Saturating burn-down decrement: see cores.md, "The 68K decoder prologue" */                                     \
    while (*instructions > 0) {                                                                                        \
        uint32_t fetch = memory_read_prefetch32(cpu->pc);                                                              \
        uint16_t opcode = fetch >> 16;                                                                                 \
        cpu->instruction_pc = cpu->pc;                                                                                 \
        cpu->pc += 2;                                                                                                  \
        if (*instructions > 0)                                                                                         \
            (*instructions)--;
#define CPU_DECODER_EPILOGUE                                                                                           \
    }                                                                                                                  \
    /* Sprint exit (cpu_sprint.h): deferred bus error (Format $B/$A), trace */                                         \
    m68k_sprint_exit(cpu, saved_trace)

#include "cpu_decode.h"
#undef CPU_DECODER_NAME
#undef CPU_DECODER_ARGS
#undef CPU_DECODER_RETURN_TYPE
#undef CPU_DECODER_PROLOGUE
#undef CPU_DECODER_EPILOGUE

// ============================================================================
// The predecoded executor (docs/internals/core/cpu/predecode.md): the
// one-instruction executor, the sprint loop over predecoded entries, and
// the decode tree in its classifier role — three more instantiations of
// the same template, sharing this file's macro bindings and op bodies.
// ============================================================================
#define PD_RUN_NAME      cpu_pd_run_68030
#define PD_STEP_NAME     cpu_pd_step_68030
#define PD_DECODE_NAME   cpu_pd_decode_68030
#define PD_TREE_NAME     cpu_pd_tree_68030
#define PD_CLASSIFY_NAME cpu_pd_classify_68030
#include "cpu_pd_run.h"

// The core's entry point: the predecoded executor when enabled, else the
// switch core (kept for A/B from the shell: machine.cpu.predecode = 0).
void cpu_run_68030(cpu_t *restrict cpu, uint32_t *instructions) {
    if (predecode_enabled())
        cpu_pd_run_68030(cpu, instructions);
    else
        cpu_run_68030_switch(cpu, instructions);
}
