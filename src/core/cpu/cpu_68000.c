// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_68000.c
// Motorola 68000 instruction decoder instantiation.
// Supplies the 68000 prologue/epilogue around the shared macro set
// (cpu_decoder_macros.h) and the cpu_ops.h / cpu_decode.h templates to
// generate cpu_run_68000().

#include "cpu_internal.h"

#include "system.h"

// Operand/memory/flag/exception macros shared by all three 68K decoders
#include "cpu_decoder_macros.h"

#include "cpu_ops.h"

// Generate the cpu_run_68000 decoder function
#define CPU_DECODER_NAME        cpu_run_68000
#define CPU_DECODER_ARGS        cpu_t *restrict cpu, uint32_t *instructions
#define CPU_DECODER_RETURN_TYPE void
// Saturating burn-down decrement at the end of the prologue: see
// docs/internals/core/cpu/cores.md, "The 68K decoder prologue".
#define CPU_DECODER_PROLOGUE                                                                                           \
    /* Double bus fault: the CPU halts, and on Mac hardware the halt line is                                           \
     * wired straight to the board's reset input, so the machine reboots.                                              \
     * Apple's Guide to the Macintosh Family Hardware documents the wiring for                                         \
     * the Mac SE -- itself a 68000 machine -- twice: "/HALT ... Tied to                                               \
     * MC68000 /RES line", and "/RESET ... Master reset for entire board; tied                                         \
     * to MC68000 /HALT line".  MC68030UM 7.5.4 gives the CPU half of the same                                         \
     * rule ("Only an external reset operation can restart a halted                                                    \
     * processor"); the board is what supplies that external reset.                                                    \
     *                                                                                                                 \
     * Deliberately IDENTICAL to the 68030 and 68040 decoders.  The 68000                                              \
     * previously just returned instead, leaving pc already advanced past the                                          \
     * faulting opcode by this prologue, so the next sprint resumed                                                    \
     * mid-instruction -- and that left the Plus and Lisa as the only machines                                         \
     * in the tree behaving differently on a double fault, with no hardware                                            \
     * basis for the difference. */                                                                                    \
    if (__builtin_expect(cpu->halted, 0)) {                                                                            \
        cpu->halted = 0;                                                                                               \
        system_reset_devices(); /* the board's /RESET net; precedes the vector read */                                 \
        cpu_reset_to_vector_68030(cpu); /* CPU half only; not 030-specific */                                          \
    }                                                                                                                  \
    cpu_check_interrupt(cpu);                                                                                          \
    /* Let a memory-layer fault (lisa_raise_bus_error / memory.c) force this sprint                                    \
     * to exit immediately by zeroing the burndown counter, so a deferred DATA bus                                     \
     * error is delivered at the FAULTING instruction's epilogue.  The 68030 decoder                                   \
     * sets this; the 68000 decoder omitted it, so *g_bus_error_instr_ptr=0 wrote                                      \
     * through a stale pointer and the sprint ran on — a user-mode data fault (e.g. a                                \
     * Lisa stack-growth fault) then leaked across ~hundreds of instructions into                                      \
     * unrelated (supervisor / MMU-setup) code, where it was delivered with the wrong                                  \
     * context and vectored through the ROM, resetting the machine. */                                                 \
    g_bus_error_instr_ptr = instructions;                                                                              \
    while (*instructions > 0) {                                                                                        \
        /* The MC68000 has a 24-bit address bus (A0-A23); bits 24-31 of the PC are                                     \
         * not driven.  Control transfers through a pointer whose high byte is a                                       \
         * tag (e.g. the Lisa OS inter-segment jump-table entries, $A0xxxxxx) rely                                     \
         * on this truncation.  Keep cpu->pc 24-bit so instruction_pc matches the                                      \
         * 24-bit fault address a demand-segment bus error reports (otherwise                                          \
         * f_trap's g_bus_error_address==instruction_pc check fails and the fault is                                   \
         * mis-delivered as a line-F instead of demand-loading the segment).  No-op                                    \
         * for the Mac Plus, whose PC never exceeds 24 bits. */                                                        \
        cpu->pc &= 0x00FFFFFFu;                                                                                        \
        uint32_t fetch = memory_read_prefetch32(cpu->pc);                                                              \
        uint16_t opcode = fetch >> 16;                                                                                 \
        /* Record the address of the instruction being decoded.  The group-0                                           \
         * exception path (bus/address error, f_trap demand-segment fault on the                                       \
         * Lisa) reads cpu->instruction_pc to build the stack frame and to match                                       \
         * the faulting fetch address in f_trap.  The 68030 decoder sets this;                                         \
         * the 68000 decoder previously omitted it, so a demand-segment fetch                                          \
         * fault (Lisa SYSTEM.SHELL seg-24 load) built its frame with a stale PC                                       \
         * (0) and mis-routed the fault.  Mirror the 68030 prologue exactly. */                                        \
        cpu->instruction_pc = cpu->pc;                                                                                 \
        /* Latch the instruction register only on a non-faulting fetch.  When the                                      \
         * fetch bus-errors (jump/call into an absent code segment), cpu->ir keeps                                     \
         * the control-transfer opcode that branched here, which the group-0 frame                                     \
         * must carry so the Lisa OS demand-segment handler can recognise it. */                                       \
        if (__builtin_expect(!g_bus_error_pending, 1)) {                                                               \
            cpu->ir = opcode;                                                                                          \
            cpu->ir_pc = cpu->instruction_pc;                                                                          \
        }                                                                                                              \
        if (__builtin_expect(cpu->last_bus_error_pc != 0 && !cpu->supervisor && cpu->last_bus_error_pc != cpu->pc, 0)) \
            cpu->last_bus_error_pc = 0;                                                                                \
        cpu->pc += 2;                                                                                                  \
        if (*instructions > 0)                                                                                         \
            (*instructions)--;
#define CPU_DECODER_EPILOGUE                                                                                           \
    }                                                                                                                  \
    /* Deferred DATA bus error.  A memory access during the instruction faulted    */                                  \
    /* (the memory layer set g_bus_error_pending and zeroed *instructions to break  */                                 \
    /* out of the sprint).  Unlike an instruction-FETCH fault — which decodes as a   */                              \
    /* line-F opcode and is delivered inline via f_trap — a data read/write fault     */                             \
    /* has no delivery point on the 68000 path.  The 68030 decoder delivers it in     */                               \
    /* its epilogue (cpu_68030.c); the 68000 path omitted this, so a Lisa demand-      */                              \
    /* segment / write-protect DATA fault left g_bus_error_pending stuck true.  Every   */                             \
    /* later -(A7)/LINK/MOVEM/JSR push then skipped its SP update — the restart-safety  */                           \
    /* guards (PUSH, write_ea, movem_from_register) treat a still-pending flag as "this */                             \
    /* push faulted, roll back" — corrupting the supervisor stack until SET_DOMAIN      */                           \
    /* popped a stale frame pointer and jumped into it.  Deliver it here as a group-0   */                             \
    /* bus error; the Lisa OS BUS_ERR handler classifies/recovers (or terminates) it.   */                             \
    if (__builtin_expect(g_bus_error_pending, 0)) {                                                                    \
        g_bus_error_pending = false;                                                                                   \
        /* Group-0 (data) bus-error saved PC = faulting instruction + 2 (just past  */                                 \
        /* the opcode word).  The decoder advanced cpu->pc to the *next* instruction */                                \
        /* during operand decode; the real 68000 stacks PC pointing 2 bytes into the */                                \
        /* faulting instruction.  Xenix's bus-error handler reads the word at         */                               \
        /* (savedPC-2) to detect the C stack-growth probe `TST.B d16(A7)` (opcode     */                               \
        /* 0x4A2F): with the next-instruction PC it read the displacement word, the   */                               \
        /* probe went undetected, and mkfs was SIGSEGV'd instead of the stack grown.  */                               \
        exception_bus_error(cpu, g_bus_error_address, g_bus_error_rw, cpu->instruction_pc + 2);                        \
        cpu_select_soa(cpu->supervisor);                                                                               \
    }                                                                                                                  \
    cpu_check_interrupt(cpu);                                                                                          \
    assert(*instructions == 0)

#include "cpu_decode.h"
