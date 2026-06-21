// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_68000.c
// Motorola 68000 instruction decoder instantiation.
// This file defines the 68000-specific memory access macros and includes
// the shared cpu_ops.h and cpu_decode.h templates to generate cpu_run_68000().

#include "cpu_internal.h"

#include "log.h"
#include "system.h"

LOG_USE_CATEGORY_NAME("cpu");

// 68000 memory access: direct (no MMU translation)
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

// Generate the cpu_run_68000 decoder function
#define CPU_DECODER_NAME        cpu_run_68000
#define CPU_DECODER_ARGS        cpu_t *restrict cpu, uint32_t *instructions
#define CPU_DECODER_RETURN_TYPE void
/* Saturating decrement on the trailing (*instructions)--: memory_io_penalty
 * can clamp *instructions to 0 during the fetch (when the I/O penalty equals
 * or exceeds the remaining burndown), and an unconditional decrement would
 * wrap to UINT32_MAX, breaking the sprint_burndown <= sprint_total invariant
 * in scheduler.c:reconcile_sprint on any SE/30 sprint that ended its last
 * instruction on a slow I/O access. */
#define CPU_DECODER_PROLOGUE                                                                                           \
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
        uint32_t fetch = memory_read_uint32(cpu->pc);                                                                  \
        uint16_t opcode = fetch >> 16;                                                                                 \
        uint16_t ext_word = fetch & 0xFFFF;                                                                            \
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
        cpu->pc = cpu->instruction_pc + 2;                                                                             \
        exception_bus_error(cpu, g_bus_error_address, g_bus_error_rw);                                                 \
        g_active_read = cpu->supervisor ? g_supervisor_read : g_user_read;                                             \
        g_active_write = cpu->supervisor ? g_supervisor_write : g_user_write;                                          \
    }                                                                                                                  \
    cpu_check_interrupt(cpu);                                                                                          \
    assert(*instructions == 0)

#include "cpu_decode.h"
