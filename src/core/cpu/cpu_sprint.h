// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_sprint.h
// Sprint entry and exit shared by the 68K cores: the switch decoders'
// CPU_DECODER_PROLOGUE/EPILOGUE (cpu_68000.c, cpu_68030.c, cpu_68040.c) and
// the predecoded run loop (cpu_pd_run.h).  Included once per core TU, after
// cpu_internal.h, log.h and system.h, with the TU's CPU_DECODER_IS_* set:
// the bodies specialise on it at compile time, so each TU gets the code it
// had before, inlined into its run function.

#ifndef CPU_SPRINT_H
#define CPU_SPRINT_H

#include "cpu_internal.h"
#include "gs_assert.h"
#include "system.h"

#if !defined(CPU_DECODER_IS_68000) && !defined(CPU_DECODER_IS_68030)
#error "cpu_sprint.h belongs to a 68K core TU (CPU_DECODER_IS_68000 or _68030)"
#endif

// Double bus fault: the CPU halts, and on Mac hardware the halt line is wired
// straight to the board's reset input, so the machine reboots (Apple's Guide
// to the Macintosh Family Hardware, for the SE: "/HALT ... Tied to MC68000
// /RES line"; MC68030UM 7.5.4: "Only an external reset operation can restart
// a halted processor").  The board's /RESET net comes first -- it re-arms the
// ROM overlay, so the vectors at $0 read ROM -- then the CPU half.  The same
// on every model: the 68000 once just returned, resuming mid-instruction.
static __attribute__((noinline, cold)) void m68k_hw_reset(cpu_t *restrict cpu) {
#ifndef CPU_DECODER_IS_68000
    LOG(1, "Hardware reset (double bus error → HALT → RESET)");
#endif
    system_reset_devices(); // the board's /RESET net; must precede the vector read
#ifdef CPU_DECODER_IS_68040
    cpu_reset_to_vector_68040(cpu);
#else
    cpu_reset_to_vector_68030(cpu); // also the 68000's: the CPU half is not 030-specific
#endif
}

// Sprint entry: reset a halted CPU, select the SoA pair for the current mode
// (a checkpoint restore or a swapped-in machine arrives with the supervisor
// pair whatever its SR says, and the Lisa MMU picks its context from it),
// let the memory slow paths end the sprint (a deferred data fault zeroes the
// counter through g_bus_error_instr_ptr, so it is delivered at the faulting
// instruction's exit), take a pending interrupt and settle the same-PC retry
// latch -- decided once per sprint (and in write_sr), never per instruction.
// Returns the trace bits to test at exit.  With T1 set the sprint is one
// instruction.  The 68000 has no trace arm (the unit corpus relies on it,
// tests/unit/suites/cpu): it returns 0.
static inline __attribute__((always_inline)) uint32_t m68k_sprint_enter(cpu_t *restrict cpu, uint32_t *instructions) {
    if (__builtin_expect(cpu->halted, 0)) {
        cpu->halted = 0;
        m68k_hw_reset(cpu);
    }
    cpu_select_soa(cpu->supervisor);
    g_bus_error_instr_ptr = instructions;
    cpu_check_interrupt(cpu);
    if (__builtin_expect(cpu->last_bus_error_pc != 0, 0))
        m68k_bus_error_latch_settle(cpu, instructions);
#ifdef CPU_DECODER_IS_68000
    return 0;
#else
    uint32_t saved_trace = cpu->trace;
    if (__builtin_expect(saved_trace & 2, 0))
        if (*instructions > 1) {
            g_sprint_io.unrun_slots += *instructions - 1; // the rest of the plan is not run
            *instructions = 1;
        }
    return saved_trace;
#endif
}

// Sprint exit: deliver the deferred bus error, else a trace, then any
// interrupt.  Exception priority (MC68030UM Table 8-5): group 1 bus error
// before group 4 trace, then interrupt; the bus error preempts a trace the
// same instruction would have raised.
static inline __attribute__((always_inline)) void m68k_sprint_exit(cpu_t *restrict cpu, uint32_t saved_trace) {
    if (__builtin_expect(g_bus_error_pending, 0)) {
        g_bus_error_pending = false;
#if defined(CPU_DECODER_IS_68040)
        // Format $7 access error: MMU descriptor faults retry (the handler maps
        // the page, RTE restarts the instruction), plain bus timeouts skip so
        // ROM probes advance past the bad access -- same split as the 030.
        if (g_bus_error_is_pmmu)
            exception_bus_error_retry(cpu, g_bus_error_address, g_bus_error_rw);
        else
            exception_bus_error(cpu, g_bus_error_address, g_bus_error_rw, cpu->pc);
#elif defined(CPU_DECODER_IS_68030)
        // PMMU table-walk failures use Format $B (retry) so the kernel's fault
        // handler restarts the instruction after fixing the PTE; plain bus
        // timeouts (unmapped physical in a NuBus-probe range) use Format $A
        // (skip).  g_bus_error_is_pmmu was set on the memory layer's g_mmu
        // while this reads cpu->mmu: boards set the two together
        // (memory_map_set_pmmu + cpu_attach_mmu), and the assert holds them
        // to it.
        GS_ASSERT(cpu->mmu == (void *)g_mmu);
        if (cpu->mmu && ((mmu_state_t *)cpu->mmu)->enabled && g_bus_error_is_pmmu)
            exception_bus_error_retry(cpu, g_bus_error_address, g_bus_error_rw);
        else
            exception_bus_error(cpu, g_bus_error_address, g_bus_error_rw, cpu->pc);
#else
        // Group-0 frame.  An address error stacks the PC the instruction had
        // reached (its end, every extension word fetched); a bus error the
        // faulting instruction + 2, as the real 68000 does: Xenix's handler
        // reads the word at (saved PC - 2) to recognise its stack-growth
        // probe `TST.B d16(A7)`.  Delivering here is what clears the pending
        // flag a Lisa data fault set (left set, every later push rolled back).
        exception_bus_error(cpu, g_bus_error_address, g_bus_error_rw,
                            g_bus_error_is_address ? cpu->pc : cpu->instruction_pc + 2);
#endif
        cpu_select_soa(cpu->supervisor);
    }
#ifndef CPU_DECODER_IS_68000
    else if (__builtin_expect((saved_trace & 2) && (cpu->trace & 2), 0)) {
        // Trace: T1 set at sprint start and still set now (for SR-modifying
        // instructions, the new T1, M68000 PRM 6.3.10)
        exception(cpu, M68K_VEC_TRACE, cpu->pc, cpu_get_sr(cpu));
    }
#endif
    (void)saved_trace;
    cpu_check_interrupt(cpu);
}

#endif // CPU_SPRINT_H
