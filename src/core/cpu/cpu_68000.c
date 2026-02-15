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
#define CPU_DECODER_PROLOGUE                                                                                           \
    cpu_check_interrupt(cpu);                                                                                          \
    while (*instructions > 0) {                                                                                        \
        uint32_t fetch = memory_read_uint32(cpu->pc);                                                                  \
        uint16_t opcode = fetch >> 16;                                                                                 \
        uint16_t ext_word = fetch & 0xFFFF;                                                                            \
        cpu->pc += 2;                                                                                                  \
        (*instructions)--;
#define CPU_DECODER_EPILOGUE                                                                                           \
    }                                                                                                                  \
    cpu_check_interrupt(cpu);                                                                                          \
    assert(*instructions == 0)

#include "cpu_decode.h"
