// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_68000.c
// Motorola 68000 instruction decoder instantiation.
// Supplies the 68000 prologue/epilogue around the shared macro set
// (cpu_decoder_macros.h) and the cpu_ops.h / cpu_decode.h templates to
// generate cpu_run_68000().

#define CPU_DECODER_IS_68000 1 // this unit is the 68000 core: odd-address checks compile unconditionally
#include "cpu_internal.h"

#include "system.h"

// Operand/memory/flag/exception macros shared by all three 68K decoders
#include "cpu_decoder_macros.h"
#include "cpu_sprint.h"
// The 68000 alone checks word/long operand addresses for oddness (an
// address error, cpu_internal.h); the 68020+ bus sizes misaligned accesses.
#undef READ16
#undef READ32
#undef WRITE16
#undef WRITE32
#define READ16(addr)     cpu_dread16(cpu, addr)
#define READ32(addr)     cpu_dread32(cpu, addr)
#define WRITE16(addr, x) cpu_dwrite16(cpu, addr, x)
#define WRITE32(addr, x) cpu_dwrite32(cpu, addr, x)

#include "cpu_ops.h"

// Generate the cpu_run_68000 decoder function
#define CPU_DECODER_NAME        cpu_run_68000_switch
#define CPU_DECODER_ARGS        cpu_t *restrict cpu, uint32_t *instructions
#define CPU_DECODER_RETURN_TYPE void
// Saturating burn-down decrement at the end of the prologue: see
// docs/internals/core/cpu/cores.md, "The 68K decoder prologue".
#define CPU_DECODER_PROLOGUE                                                                                           \
    /* Sprint entry (cpu_sprint.h): the 68000 has no trace arm, so saved_trace is 0 */                                 \
    uint32_t saved_trace = m68k_sprint_enter(cpu, instructions);                                                       \
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
        if (__builtin_expect(cpu->pc & 1u, 0)) { /* odd PC: address error, the epilogue delivers */                    \
            m68k_fetch_address_error(cpu);                                                                             \
            break;                                                                                                     \
        }                                                                                                              \
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
        cpu->pc += 2;                                                                                                  \
        if (*instructions > 0)                                                                                         \
            (*instructions)--;
#define CPU_DECODER_EPILOGUE                                                                                           \
    }                                                                                                                  \
    /* Sprint exit (cpu_sprint.h): the deferred data bus / address error */                                            \
    m68k_sprint_exit(cpu, saved_trace)

// MOVES does not exist on the 68000: both direction arms are OP_UNDEFINED, so
// don't let cpu_decode.h's live extension-word read issue a bus cycle the
// real chip never makes.
#define CPU_MOVES_DIR() 0

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
#define PD_RUN_NAME      cpu_pd_run_68000
#define PD_STEP_NAME     cpu_pd_step_68000
#define PD_DECODE_NAME   cpu_pd_decode_68000
#define PD_TREE_NAME     cpu_pd_tree_68000
#define PD_CLASSIFY_NAME cpu_pd_classify_68000
#include "cpu_pd_run.h"

// The core's entry point: the predecoded executor when enabled, else the
// switch core (kept for A/B from the shell: machine.cpu.predecode = 0).
void cpu_run_68000(cpu_t *restrict cpu, uint32_t *instructions) {
    if (predecode_enabled() && g_lisa_mmu == NULL)
        cpu_pd_run_68000(cpu, instructions);
    else
        cpu_run_68000_switch(cpu, instructions);
}
