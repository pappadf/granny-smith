// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_ops.h
// Macros implementing the Motorola 680xx instruction set.

#ifndef CPU_OPS_H
#define CPU_OPS_H

#include <stdint.h>

#if !defined(D) || !defined(A)
#error "Data/Address register access macros not defined"
#endif

#if !defined(CC_C) || !defined(CC_X) || !defined(CC_N) || !defined(CC_V) || !defined(CC_Z)
#error "Condition codes not defined"
#endif

#if !defined(READ8) || !defined(READ16) || !defined(READ32) || !defined(WRITE8) || !defined(WRITE16) ||                \
    !defined(WRITE32)
#error "Memory access macros not defined"
#endif

#if !defined(FETCH8) || !defined(FETCH16) || !defined(FETCH32)
#error "Instruction stream fetch macros not defined"
#endif

#if !defined(FETCH16_NO_INC) || !defined(FETCH32_NO_INC)
#error "Instruction prefetch macros not defined"
#endif

#if !defined(GET_SR) || !defined(SET_SR)
#error "Status register macros not defined"
#endif

#if !defined(GET_USP) || !defined(SET_USP)
#error "User stack pointer macros not defined"
#endif

#if !defined(READ_CCR) || !defined(WRITE_CCR)
#error "Condition code register macros not defined"
#endif

#if !defined(SBCD) || !defined(ABCD)
#error "BCD operation macros not defined"
#endif

#if !defined(MOVEM_FROM_REGISTER) || !defined(MOVEM_TO_REGISTER)
#error "MOVEM helper macros not defined"
#endif

#if !defined(READ_EA) || !defined(WRITE_EA) || !defined(CALCULATE_EA)
#error "Effective address macros not defined"
#endif

#if !defined(CONDITIONAL_TEST)
#error "Conditional test macro not defined"
#endif

#if !defined(EXC_TRAP) || !defined(EXC_TRAPV) || !defined(EXC_ATRAP) || !defined(EXC_FTRAP)
#error "Trap helper macros not defined"
#endif

#if !defined(EXC_DIVIDE_BY_ZERO) || !defined(EXC_CHK) || !defined(EXC_PRIVILEGE)
#error "Exception helper macros not defined"
#endif

#if !defined(IS_SUPERVISOR)
#error "Supervisor state macro not defined"
#endif

#if !defined(EXC_ILLEGAL)
#error "Illegal-instruction helper macro not defined"
#endif

#define UINT(bits) uint##bits##_t
#define INT(bits)  int##bits##_t

#define DN D(opcode >> 9 & 7)
#define DX D(opcode >> 9 & 7)
#define DY D(EA_REG)

#define AN A(opcode >> 9 & 7)
#define AX A(opcode >> 9 & 7)
#define AY A(EA_REG)

#define SP (A(7))

// Alternate-byte accesses (stride 2) used by MOVEP
#define READ2x8(addr)     (((uint16_t)READ8(addr) << 8) | (uint16_t)READ8((addr) + 2))
#define READ4x8(addr)     (((uint32_t)READ2x8(addr) << 16) | (uint32_t)READ2x8((addr) + 4))
#define WRITE2x8(addr, x) (WRITE8(addr, ((x) >> 8) & 0xFF), WRITE8((addr) + 2, (x) & 0xFF))
#define WRITE4x8(addr, x) (WRITE2x8(addr, ((x) >> 16) & 0xFFFF), WRITE2x8((addr) + 4, (x) & 0xFFFF))

// Branch displacements.  DISP8 is the Bcc.B/BSR.B byte in the opcode; the
// decoder has already routed $00 (word form) and $FF (long form) elsewhere,
// so callers must not use it on any other opcode.
#define DISP8()  ((int32_t)(int8_t)(opcode & 0xFF))
#define DISP16() ((int32_t)(int16_t)FETCH16())
#define DISP32() ((int32_t)FETCH32())

// We use the term "load" when reading data into a new variable
#define LOAD_IMM(bits, var) UINT(bits) var = FETCH##bits()

// Helper macros to extract bit fields from opcode
#define EA_MODE ((opcode >> 3) & 7)
#define EA_REG  (opcode & 7)
#define DATA    quick_data(opcode)

// ADDQ/SUBQ/shift-immediate count from opcode bits 11:9, where 0 encodes 8.
// Branch-free: (n - 1) & 7 maps 0 to 7 and every other n to n - 1.
static inline int quick_data(uint16_t op) {
    return ((((op >> 9) & 7) - 1) & 7) + 1;
}

// EA-mode validation.  VALID_EA(modes) checks the opcode's own EA field;
// VALID_EA_MOVE the MOVE destination field; both go through the per-model
// VALIDATE_EA_MODE_REG, which raises the illegal-instruction exception and
// ends the instruction.  These expand to `continue`, so they MUST be used
// inside the decoder's while-loop (cpu_decode.h), as every op macro is: a
// `break` would only leave the innermost switch (cores.md, "The interpreter
// loop has exactly one exit").
#ifdef CPU_DECODER_IS_68030
// 68030 EA validation: skip extension words before illegal instruction.
// On 68030, the CPU prefetches EA extension words before detecting invalid
// modes. This override advances cpu->pc past those words on the exception
// path only — zero cost on the normal (non-exception) fast path.
#define VALIDATE_EA_68030(supported_modes, mode, reg, sz)                                                              \
    if (!((supported_modes) & 1u << ((mode) + ((mode) == 7 ? (reg) : 0)))) {                                           \
        skip_ea_extension_words(cpu, (mode), (reg), (sz));                                                             \
        EXC_ILLEGAL();                                                                                                 \
        continue;                                                                                                      \
    }
#define VALIDATE_EA_MODE_REG(supported_modes, mode, reg) VALIDATE_EA_68030(supported_modes, mode, reg, 0)
// Override LOAD_EA to pass correct operand size for immediate mode skipping
#define LOAD_EA(bits, x, modes)                                                                                        \
    VALIDATE_EA_68030(modes, EA_MODE, EA_REG, (bits) / 8);                                                             \
    UINT(bits) x = READ_EA(bits, opcode, false)
#else
#define VALIDATE_EA_68000(supported_modes, mode, reg)                                                                  \
    if (!((supported_modes) & 1u << ((mode) + ((mode) == 7 ? (reg) : 0)))) {                                           \
        EXC_ILLEGAL();                                                                                                 \
        continue;                                                                                                      \
    }
#define VALIDATE_EA_MODE_REG(supported_modes, mode, reg) VALIDATE_EA_68000(supported_modes, mode, reg)
#define LOAD_EA(bits, x, modes)                                                                                        \
    VALID_EA(modes);                                                                                                   \
    UINT(bits) x = READ_EA(bits, opcode, false)
#endif
#define VALID_EA(modes)              VALIDATE_EA_MODE_REG(modes, EA_MODE, EA_REG)
#define VALID_EA_MOVE(modes)         VALIDATE_EA_MODE_REG(modes, (opcode >> 6) & 7, (opcode >> 9) & 7)
#define LOAD_EA_WITH_UPDATE(bits, x) UINT(bits) x = READ_EA(bits, opcode, true)

// load from -(An), i.e. An in pre-decrement mode.  An is decremented BEFORE
// the read and is not restored if the read faults: a caller whose instruction
// can be retried (Format $B) must snapshot An first and roll it back when
// g_bus_error_pending is set, as SUBX/ADDX/ABCD/SBCD/CMPM/PACK/UNPK do.
#define LOAD_AN8_PREDEC(var, n)                                                                                        \
    A(n) -= (n) == 7 ? 2 : 1;                                                                                          \
    uint8_t var = READ8(A(n))
#define LOAD_AN16_PREDEC(var, n)                                                                                       \
    A(n) -= 2;                                                                                                         \
    uint16_t var = READ16(A(n))
#define LOAD_AN32_PREDEC(var, n)                                                                                       \
    A(n) -= 4;                                                                                                         \
    uint32_t var = READ32(A(n))
#define LOAD_AN_PREDEC(bits, var, n) LOAD_AN##bits##_PREDEC(var, n)

#define LOAD_AN8_POSTINC(var, n)                                                                                       \
    uint8_t var = READ8(A(n));                                                                                         \
    A(n) += (n) == 7 ? 2 : 1;
#define LOAD_AN16_POSTINC(var, n)                                                                                      \
    uint16_t var = READ16(A(n));                                                                                       \
    A(n) += 2;
#define LOAD_AN32_POSTINC(var, n)                                                                                      \
    uint32_t var = READ32(A(n));                                                                                       \
    A(n) += 4;
#define LOAD_AN_POSTINC(bits, var, n) LOAD_AN##bits##_POSTINC(var, n)

#define STORE_DN8(n, value)      D(n) = (D(n) & 0xFFFFFF00) | (uint8_t)((value) & 0xFF)
#define STORE_DN16(n, value)     D(n) = (D(n) & 0xFFFF0000) | (uint16_t)(value)
#define STORE_DN32(n, value)     D(n) = (value)
#define STORE_DN(bits, n, value) STORE_DN##bits(n, value)

#define STORE_EA(bits, res) WRITE_EA(bits, EA_MODE, EA_REG, res)

#define STORE_AT_AN(bits, n, res) WRITE##bits(A(n), res)
#define CC                        CONDITIONAL_TEST(opcode >> 8 & 0xF)

#define BITS(x)         (sizeof(x) * 8)
#define MSB(x)          ((x >> (BITS(x) - 1)) & 1) // most significant bit
#define S_EXT_8TO32(x)  ((int32_t)(int8_t)(x)) // sign-extend 8-bit to 32-bit
#define S_EXT_16TO32(x) ((int32_t)(int16_t)(x)) // sign-extend 16-bit to 32-bit
#define S_EXT_8TO16(x)  ((int16_t)(int8_t)(x)) // sign-extend 8-bit to 16-bit

// Generic helper macros to clear condition codes
#define CLEAR_N()    (CC_N = 0)
#define CLEAR_NZVC() (CC_N = CC_Z = CC_V = CC_C = 0)

// Generic helper macros to update condition codes based on result
// (Each is one parenthesised expression, so it is safe as the body of an if.)
#define UPDATE_N(res)           (CC_N = (res) & 1u << (BITS(res) - 1))
#define UPDATE_Z(res)           (CC_Z = !(res))
#define UPDATE_NZ_CLEAR_V(res)  (UPDATE_N(res), UPDATE_Z(res), CC_V = 0)
#define UPDATE_NZ_CLEAR_CV(res) (UPDATE_N(res), UPDATE_Z(res), CC_C = CC_V = 0)

// Helper macros to update condition codes for specific operations
#define UPDATE_V_SUB(dst, src, res)        CC_V = ((((dst) ^ (src)) & ((dst) ^ (res))) >> (BITS(res) - 1))
#define UPDATE_C_SUB(dst, src, res)        CC_C = (res) > (dst)
#define UPDATE_CX_SUBX(dst, src, res)      CC_C = CC_X = (((~dst & src) | ((~dst | src) & res)) >> (BITS(res) - 1))
#define UPDATE_V_ADD(dst, src, res)        CC_V = ((~((dst) ^ (src)) & ((dst) ^ (res))) >> (BITS(res) - 1))
#define UPDATE_C_ADD(dst, src, res)        CC_C = (res) < (dst)
#define UPDATE_CX_ADDX(dst, src, res)      CC_C = CC_X = (((dst & src) | ((dst | src) & ~res)) >> (BITS(res) - 1))
#define UPDATE_X_SHIFT(count)              CC_X = (count && CC_C) || (!count && CC_X)
#define UPDATE_C_SHIFT_L(data, count)      CC_C = count && (count <= BITS(data)) && (data & 1u << (BITS(data) - count))
#define UPDATE_C_SHIFT_R(data, count, res) CC_C = count && (count > BITS(data) ? res : data & 1u << (count - 1))

// Generic SUB.  The UPDATE_V/N/Z steps never touch C, so SUB/ADD can copy
// the finished C into X last (CC_X = CC_C) -- keep it that way.
#define GENERIC_SUB(dst, src, res)                                                                                     \
    res = dst - src;                                                                                                   \
    UPDATE_C_SUB(dst, src, res);                                                                                       \
    UPDATE_V_SUB(dst, src, res);                                                                                       \
    UPDATE_N(res);                                                                                                     \
    UPDATE_Z(res);

#define SUB(bits, dst, src, res)                                                                                       \
    UINT(bits) res;                                                                                                    \
    GENERIC_SUB(dst, src, res);                                                                                        \
    CC_X = CC_C;

#define SUB_EA_DN(bits, mode)                                                                                          \
    VALID_EA(mode);                                                                                                    \
    LOAD_EA_WITH_UPDATE(bits, src);                                                                                    \
    SUB(bits, (UINT(bits))DN, src, res);                                                                               \
    STORE_DN(bits, opcode >> 9 & 7, res);

#define SUB_DN_EA(bits)                                                                                                \
    LOAD_EA(bits, dst, (ea_memory & ea_alterable));                                                                    \
    SUB(bits, dst, (UINT(bits))DN, res);                                                                               \
    STORE_EA(bits, res);

// On 68000: validate EA before immediate fetch so illegal_instruction() stacks the
// correct PC (cpu->pc - 2 = instruction address, before immediate was consumed).
// On 68030: fetch immediate first so skip_ea_extension_words() has correct PC.
#ifdef CPU_DECODER_IS_68030
#define SUBI(bits)                                                                                                     \
    LOAD_IMM(bits, src);                                                                                               \
    LOAD_EA(bits, dst, (ea_data & ea_alterable));                                                                      \
    SUB(bits, dst, src, res);                                                                                          \
    STORE_EA(bits, res);

// ORI/ANDI/EORI.[BWL] #<data>,<ea>
#define IMM_LOGICAL(bits, op)                                                                                          \
    LOAD_IMM(bits, src);                                                                                               \
    LOAD_EA(bits, dst, (ea_data & ea_alterable));                                                                      \
    UINT(bits) res = dst op src;                                                                                       \
    UPDATE_NZ_CLEAR_CV(res);                                                                                           \
    STORE_EA(bits, res);

// ADDI.[BWL] #<data>,<ea>
#define ADDI(bits)                                                                                                     \
    LOAD_IMM(bits, src);                                                                                               \
    LOAD_EA(bits, dst, (ea_data & ea_alterable));                                                                      \
    ADD(bits, dst, src, res);                                                                                          \
    STORE_EA(bits, res);

// CMPI.[BWL] #<data>,<ea>
// 68030: PC-relative modes supported; fetch immediate first for correct exception PC.
#define CMPI(bits)                                                                                                     \
    LOAD_IMM(bits, src);                                                                                               \
    VALID_EA(ea_data & ~ea_xxx);                                                                                       \
    LOAD_EA_WITH_UPDATE(bits, dst);                                                                                    \
    uint##bits##_t res;                                                                                                \
    GENERIC_SUB(dst, src, res);
#else
#define SUBI(bits)                                                                                                     \
    VALID_EA((ea_data & ea_alterable));                                                                                \
    LOAD_IMM(bits, src);                                                                                               \
    LOAD_EA(bits, dst, (ea_data & ea_alterable));                                                                      \
    SUB(bits, dst, src, res);                                                                                          \
    STORE_EA(bits, res);

// ORI/ANDI/EORI.[BWL] #<data>,<ea>
#define IMM_LOGICAL(bits, op)                                                                                          \
    VALID_EA((ea_data & ea_alterable));                                                                                \
    LOAD_IMM(bits, src);                                                                                               \
    LOAD_EA(bits, dst, (ea_data & ea_alterable));                                                                      \
    UINT(bits) res = dst op src;                                                                                       \
    UPDATE_NZ_CLEAR_CV(res);                                                                                           \
    STORE_EA(bits, res);

// ADDI.[BWL] #<data>,<ea>
#define ADDI(bits)                                                                                                     \
    VALID_EA((ea_data & ea_alterable));                                                                                \
    LOAD_IMM(bits, src);                                                                                               \
    LOAD_EA(bits, dst, (ea_data & ea_alterable));                                                                      \
    ADD(bits, dst, src, res);                                                                                          \
    STORE_EA(bits, res);

// CMPI.[BWL] #<data>,<ea>
// 68000: PC-relative modes not supported; validate before immediate fetch.
#define CMPI(bits)                                                                                                     \
    VALID_EA(ea_data - ea_xxx - ea_d16_pc - ea_d8_pc_xn);                                                              \
    LOAD_IMM(bits, src);                                                                                               \
    LOAD_EA_WITH_UPDATE(bits, dst);                                                                                    \
    uint##bits##_t res;                                                                                                \
    GENERIC_SUB(dst, src, res);
#endif

// SUBQ.[BWL] #<data>,<ea>
#define SUBQ(bits)                                                                                                     \
    UINT(bits) src = DATA;                                                                                             \
    LOAD_EA(bits, dst, ea_alterable - ea_an);                                                                          \
    SUB(bits, dst, src, res);                                                                                          \
    STORE_EA(bits, res);

#define SUBX(dst, src, res)                                                                                            \
    res = dst - src - (CC_X ? 1 : 0);                                                                                  \
    UPDATE_CX_SUBX(dst, src, res);                                                                                     \
    UPDATE_V_SUB(dst, src, res);                                                                                       \
    UPDATE_N(res);                                                                                                     \
    CC_Z = CC_Z && (res == 0);

// SUBX.[BWL] Dx,Dy
#define SUBX_DX_DY(bits)                                                                                               \
    uint##bits##_t dst = DX, src = DY, res;                                                                            \
    SUBX(dst, src, res);                                                                                               \
    STORE_DN(bits, opcode >> 9 & 7, res);

// SUBX.[BWL] -(Ay),-(Ax): each predec decrements An before its read, and the
// final store writes to the (already-decremented) Ax.  A fault on either read
// or the write would leak the decrement(s) into the Format-$B retry, so
// snapshot both An up-front and roll back on bus error.
#define SUBX_AX_AY(bits)                                                                                               \
    uint32_t subx_ay_save_ = cpu->a[EA_REG];                                                                           \
    uint32_t subx_ax_save_ = cpu->a[(opcode >> 9) & 7];                                                                \
    LOAD_AN_PREDEC(bits, src, EA_REG);                                                                                 \
    LOAD_AN_PREDEC(bits, dst, opcode >> 9 & 7);                                                                        \
    UINT(bits) res;                                                                                                    \
    SUBX(dst, src, res);                                                                                               \
    STORE_AT_AN(bits, opcode >> 9 & 7, res);                                                                           \
    if (__builtin_expect(g_bus_error_pending, 0)) {                                                                    \
        cpu->a[EA_REG] = subx_ay_save_;                                                                                \
        cpu->a[(opcode >> 9) & 7] = subx_ax_save_;                                                                     \
    }

#define CMP_EA_DN(bits, mode)                                                                                          \
    VALID_EA(mode);                                                                                                    \
    LOAD_EA_WITH_UPDATE(bits, src);                                                                                    \
    UINT(bits) res;                                                                                                    \
    GENERIC_SUB(((UINT(bits))DN), src, res);

#define CMPA_EA_AN(bits)                                                                                               \
    VALID_EA(ea_any);                                                                                                  \
    LOAD_EA_WITH_UPDATE(bits, src);                                                                                    \
    UINT(32) res;                                                                                                      \
    GENERIC_SUB(AN, (uint32_t)(int32_t)(INT(bits))src, res);

// CMPM (Ay)+,(Ax)+: both reads post-increment their An, so a fault on either
// access leaks the increment(s) into the Format-$B retry.  Snapshot both Ay
// and Ax up-front and restore on bus error so the retry restarts clean.
#define CMPM_AY_AX(bits)                                                                                               \
    uint32_t cmpm_ay_save_ = cpu->a[EA_REG];                                                                           \
    uint32_t cmpm_ax_save_ = cpu->a[(opcode >> 9) & 7];                                                                \
    LOAD_AN_POSTINC(bits, src, EA_REG);                                                                                \
    LOAD_AN_POSTINC(bits, dst, opcode >> 9 & 7);                                                                       \
    if (__builtin_expect(g_bus_error_pending, 0)) {                                                                    \
        cpu->a[EA_REG] = cmpm_ay_save_;                                                                                \
        cpu->a[(opcode >> 9) & 7] = cmpm_ax_save_;                                                                     \
    }                                                                                                                  \
    UINT(bits) res;                                                                                                    \
    GENERIC_SUB(dst, src, res);

// ANDI/ORI/EORI.B #<data>,CCR
#define TO_CCR(operand)                                                                                                \
    LOAD_IMM(8, src);                                                                                                  \
    WRITE_CCR(READ_CCR() operand src);

// ANDI/ORI/EORI.W #<data>,SR
#define TO_SR(operand) SUPER(LOAD_IMM(16, src); SET_SR(GET_SR() operand src))

// Generic ADD
#define GENERIC_ADD(dst, src, res)                                                                                     \
    res = dst + src;                                                                                                   \
    UPDATE_C_ADD(dst, src, res);                                                                                       \
    UPDATE_V_ADD(dst, src, res);                                                                                       \
    UPDATE_N(res);                                                                                                     \
    UPDATE_Z(res);

#define ADD(bits, dst, src, res)                                                                                       \
    UINT(bits) res;                                                                                                    \
    GENERIC_ADD(dst, src, res);                                                                                        \
    CC_X = CC_C;

// ADD.[BWL] <ea>,Dn
#define ADD_EA_DN(bits, mode)                                                                                          \
    UINT(bits) dst = DN;                                                                                               \
    VALID_EA(mode);                                                                                                    \
    LOAD_EA_WITH_UPDATE(bits, src);                                                                                    \
    ADD(bits, dst, src, res);                                                                                          \
    STORE_DN(bits, opcode >> 9 & 7, res);

// ADD.[BWL] Dn,<ea>
#define ADD_DN_EA(bits)                                                                                                \
    LOAD_EA(bits, dst, (ea_memory & ea_alterable));                                                                    \
    UINT(bits) src = DN;                                                                                               \
    ADD(bits, dst, src, res);                                                                                          \
    STORE_EA(bits, res);

// ADDQ.[BWL] #<data>,<ea>
#define ADDQ(bits)                                                                                                     \
    UINT(bits) src = DATA;                                                                                             \
    LOAD_EA(bits, dst, ea_alterable - ea_an);                                                                          \
    ADD(bits, dst, src, res);                                                                                          \
    STORE_EA(bits, res);

#define ADDX(dst, src, res)                                                                                            \
    res = dst + src + (CC_X ? 1 : 0);                                                                                  \
    UPDATE_CX_ADDX(dst, src, res);                                                                                     \
    UPDATE_V_ADD(dst, src, res);                                                                                       \
    UPDATE_N(res);                                                                                                     \
    CC_Z = CC_Z && (res == 0);

#define ADDX_DX_DY(bits)                                                                                               \
    UINT(bits) dst = DX, src = DY, res;                                                                                \
    ADDX(dst, src, res);                                                                                               \
    STORE_DN(bits, opcode >> 9 & 7, res);

// ADDX.[BWL] -(Ay),-(Ax): same restart-safety concern as SUBX_AX_AY.
#define ADDX_AX_AY(bits)                                                                                               \
    uint32_t addx_ay_save_ = cpu->a[EA_REG];                                                                           \
    uint32_t addx_ax_save_ = cpu->a[(opcode >> 9) & 7];                                                                \
    LOAD_AN_PREDEC(bits, src, EA_REG);                                                                                 \
    LOAD_AN_PREDEC(bits, dst, opcode >> 9 & 7);                                                                        \
    UINT(bits) res;                                                                                                    \
    ADDX(dst, src, res);                                                                                               \
    STORE_AT_AN(bits, opcode >> 9 & 7, res);                                                                           \
    if (__builtin_expect(g_bus_error_pending, 0)) {                                                                    \
        cpu->a[EA_REG] = addx_ay_save_;                                                                                \
        cpu->a[(opcode >> 9) & 7] = addx_ax_save_;                                                                     \
    }

#define NEGX(bits)                                                                                                     \
    LOAD_EA(bits, dst, (ea_data & ea_alterable));                                                                      \
    UINT(bits) res = 0 - dst - (CC_X ? 1 : 0);                                                                         \
    CC_C = CC_X = (dst | res) >> (bits - 1) & 1;                                                                       \
    CC_Z = CC_Z && (res == 0);                                                                                         \
    CC_V = (res & dst) >> (bits - 1) & 1;                                                                              \
    UPDATE_N(res);                                                                                                     \
    STORE_EA(bits, res);

#define NEG(bits)                                                                                                      \
    LOAD_EA(bits, dst, (ea_data & ea_alterable));                                                                      \
    UINT(bits) res = 0 - dst;                                                                                          \
    STORE_EA(bits, res);                                                                                               \
    UPDATE_Z(res);                                                                                                     \
    UPDATE_N(res);                                                                                                     \
    CC_C = CC_X = (dst > 0);                                                                                           \
    CC_V = (dst & res) >> (bits - 1) & 1;

// ABCD/SBCD -(Ay),-(Ax): same restart-safety concern as SUBX_AX_AY — both
// predec reads and the final store can fault, leaking An decrements into the
// retry.  Snapshot both An and roll back on bus error.
#define XBCD_AY_AX(op)                                                                                                 \
    uint32_t xbcd_ay_save_ = cpu->a[EA_REG];                                                                           \
    uint32_t xbcd_ax_save_ = cpu->a[(opcode >> 9) & 7];                                                                \
    LOAD_AN8_PREDEC(src, EA_REG);                                                                                      \
    LOAD_AN8_PREDEC(dst, opcode >> 9 & 7);                                                                             \
    uint8_t res = op(dst, src);                                                                                        \
    STORE_AT_AN(8, opcode >> 9 & 7, res);                                                                              \
    if (__builtin_expect(g_bus_error_pending, 0)) {                                                                    \
        cpu->a[EA_REG] = xbcd_ay_save_;                                                                                \
        cpu->a[(opcode >> 9) & 7] = xbcd_ax_save_;                                                                     \
    }

#define XBCD_DY_DX(op) STORE_DN(8, opcode >> 9 & 7, op(DX, DY));

// Shared helpers for logical instructions that combine Dn with an effective address
#define LOGICAL_EA_DN(bits, op, modes)                                                                                 \
    UINT(bits) dst = DN;                                                                                               \
    VALID_EA(modes);                                                                                                   \
    LOAD_EA_WITH_UPDATE(bits, src);                                                                                    \
    UINT(bits) res = dst op src;                                                                                       \
    UPDATE_NZ_CLEAR_CV(res);                                                                                           \
    STORE_DN(bits, opcode >> 9 & 7, res)

#define LOGICAL_DN_EA(bits, op, modes)                                                                                 \
    LOAD_EA(bits, dst, modes);                                                                                         \
    UINT(bits) src = DN;                                                                                               \
    UINT(bits) res = dst op src;                                                                                       \
    UPDATE_NZ_CLEAR_CV(res);                                                                                           \
    STORE_EA(bits, res)

#define MUL_W(type)                                                                                                    \
    VALID_EA(ea_data);                                                                                                 \
    LOAD_EA_WITH_UPDATE(16, src);                                                                                      \
    type##16_t dst = DX;                                                                                               \
    type##32_t res = (type##32_t)(type##16_t)src * (type##32_t)dst;                                                    \
    UPDATE_NZ_CLEAR_CV(res);                                                                                           \
    DX = res;

#define EXG(rx, ry)                                                                                                    \
    uint32_t tmp = rx;                                                                                                 \
    rx = ry;                                                                                                           \
    ry = tmp;

#define SHIFT_EA(op)                                                                                                   \
    LOAD_EA(16, ea, (ea_memory & ea_alterable));                                                                       \
    UINT(16) res = op;                                                                                                 \
    UPDATE_NZ_CLEAR_V(res);                                                                                            \
    STORE_EA(16, res);

#define SHIFT_EA_R(op)                                                                                                 \
    SHIFT_EA(op);                                                                                                      \
    CC_C = ea & 1;

#define SHIFT_EA_L(op)                                                                                                 \
    SHIFT_EA(op);                                                                                                      \
    CC_C = ea >> 15;

// Common shift operation structure: load data, compute result, store
#define SHIFT_COMMON(bits, data, count, op)                                                                            \
    UINT(bits) d = data;                                                                                               \
    UINT(bits) c = count;                                                                                              \
    UINT(bits) r = op;                                                                                                 \
    STORE_DN(bits, EA_REG, r);

#define SHIFT_RIGHT(bits, data, count, op)                                                                             \
    SHIFT_COMMON(bits, data, count, op);                                                                               \
    UPDATE_C_SHIFT_R(d, c, r);                                                                                         \
    UPDATE_X_SHIFT(c);                                                                                                 \
    UPDATE_NZ_CLEAR_V(r);

// Arithmetic shift left.  Both shift counts in the V term are masked to the
// operand width: a count >= bits is reachable (DX & $3F yields 0-63, and
// ASL.B #8 yields 8), which makes the inner >> c and the then-negative
// (bits - c - 1) undefined.  The || short-circuits whenever d != 0, so the
// UB is only reached with d == 0, where the accidental answer happens to be
// correct -- latent rather than wrong.  Masking is a no-op for every count
// below the width, so defined inputs are unaffected.
#define ASHIFT_LEFT(bits, data, count, op)                                                                             \
    SHIFT_COMMON(bits, data, count, op);                                                                               \
    UPDATE_C_SHIFT_L(d, c);                                                                                            \
    UPDATE_X_SHIFT(c);                                                                                                 \
    UPDATE_N(r);                                                                                                       \
    UPDATE_Z(r);                                                                                                       \
    /* V: the MSB changed at some point during the shift.  asl_sign_ is d's */                                         \
    /* sign bit smeared over the top c+1 bits; any of those bits of d that */                                          \
    /* differ from it survive the final shift and make asl_flip_ non-zero. */                                          \
    UINT(bits) asl_sign_ = (UINT(bits))((INT(bits))((1u << (bits - 1)) & d) >> (c & (bits - 1)));                      \
    UINT(bits) asl_flip_ = (UINT(bits))((asl_sign_ ^ d) >> ((bits - c - 1) & (bits - 1)));                             \
    CC_V = (!r && d) || asl_flip_;

#define LSHIFT_LEFT(bits, data, count, op)                                                                             \
    SHIFT_COMMON(bits, data, count, op);                                                                               \
    UPDATE_C_SHIFT_L(d, c);                                                                                            \
    UPDATE_X_SHIFT(c);                                                                                                 \
    UPDATE_NZ_CLEAR_V(r);

// Common rotate structure: compute shift amount, rotate, update flags
#define ROTATE_COMMON(bits, data, count, shift_expr, carry_expr)                                                       \
    UINT(bits) d = (data);                                                                                             \
    UINT(bits) c = (count);                                                                                            \
    UINT(bits) s = c & ((bits) - 1);                                                                                   \
    UINT(bits) r = shift_expr;                                                                                         \
    STORE_DN(bits, EA_REG, r);                                                                                         \
    CC_C = c ? carry_expr : 0;                                                                                         \
    UPDATE_NZ_CLEAR_V(r)

/* rotate right by 'count' in an unsigned <bits>-wide value */
#define ROTATE_RIGHT(bits, data, count)                                                                                \
    ROTATE_COMMON(bits, data, count, (d >> s) | (d << ((bits - s) & ((bits) - 1))), (r >> ((bits) - 1)) & 1)

/* rotate left by 'count' in an unsigned <bits>-wide value */
#define ROTATE_LEFT(bits, data, count)                                                                                 \
    ROTATE_COMMON(bits, data, count, (d << s) | (d >> ((bits - s) & ((bits) - 1))), r & 1)

#define ROXR(bits, data, count)                                                                                        \
    UINT(bits) d = data;                                                                                               \
    UINT(bits) c = count;                                                                                              \
    UINT(bits)                                                                                                         \
    r = (c < bits ? d >> c : 0) | (c > 1 ? d << (bits + 1 - c) : 0) | (CC_X && c > 0 ? 1u << (bits - c) : 0);          \
    STORE_DN(bits, EA_REG, r);                                                                                         \
    UPDATE_NZ_CLEAR_V(r);                                                                                              \
    CC_C = CC_X = c ? d & (1u << (c - 1)) : CC_X;

#define ROXL(bits, data, count)                                                                                        \
    UINT(bits) d = data;                                                                                               \
    UINT(bits) c = count;                                                                                              \
    UINT(bits)                                                                                                         \
    r = (c < bits ? d << c : 0) | (c > 1 ? d >> (bits + 1 - c) : 0) | (CC_X && c > 0 ? 1u << (c - 1) : 0);             \
    STORE_DN(bits, EA_REG, r);                                                                                         \
    UPDATE_NZ_CLEAR_V(r);                                                                                              \
    CC_C = CC_X = c ? d & (1u << (bits - c)) : CC_X;

#define DIV16U                                                                                                         \
    VALID_EA(ea_data);                                                                                                 \
    LOAD_EA_WITH_UPDATE(16, divisor);                                                                                  \
    UINT(32) dividend = DN;                                                                                            \
    CLEAR_NZVC();                                                                                                      \
    if (!divisor) {                                                                                                    \
        EXC_DIVIDE_BY_ZERO();                                                                                          \
    } else {                                                                                                           \
        uint32_t quotient = dividend / (uint16_t)divisor;                                                              \
        if (quotient > UINT16_MAX) {                                                                                   \
            CC_V = CC_N = 1;                                                                                           \
        } else {                                                                                                       \
            uint32_t remainder = dividend % (uint16_t)divisor;                                                         \
            DX = (remainder << 16) | (quotient & 0xFFFF);                                                              \
            CC_N = quotient & 0x8000;                                                                                  \
            CC_Z = (quotient == 0);                                                                                    \
        }                                                                                                              \
    }

#define DIV16S                                                                                                         \
    VALID_EA(ea_data);                                                                                                 \
    LOAD_EA_WITH_UPDATE(16, divisor);                                                                                  \
    INT(32) dividend = (INT(32))DN;                                                                                    \
    CLEAR_NZVC();                                                                                                      \
    if (!divisor) {                                                                                                    \
        EXC_DIVIDE_BY_ZERO();                                                                                          \
    } else if ((int16_t)divisor == -1 && dividend == INT32_MIN) {                                                      \
        /* INT32_MIN / -1 has no representable quotient.  The C division is UB,                                        \
         * and the shipping wasm build's i32.div_s traps on it by specification,                                       \
         * so the test has to precede the divide rather than inspect its result. */                                    \
        CC_V = CC_N = 1;                                                                                               \
    } else {                                                                                                           \
        int32_t q = dividend / (int16_t)divisor;                                                                       \
        if (q > INT16_MAX || q < INT16_MIN) {                                                                          \
            CC_V = CC_N = 1;                                                                                           \
        } else {                                                                                                       \
            int32_t remainder = dividend % (int16_t)divisor;                                                           \
            DX = ((uint32_t)remainder << 16) | ((uint32_t)q & 0xFFFF);                                                 \
            CC_N = q & 0x8000;                                                                                         \
            CC_Z = (q == 0);                                                                                           \
        }                                                                                                              \
    }

// Generic bit operation helpers - all bit manipulation instructions follow the same pattern
// with different operations (XOR, AND-NOT, OR) and optional write-back
#define BIT_OP_WRITE(size, bit, operation)                                                                             \
    VALID_EA((ea_data & ea_alterable));                                                                                \
    UINT(size) mask = 1u << ((bit) & ((size) - 1)); /* bit < size even if a caller forgets to mask */                  \
    LOAD_EA(size, dst, (ea_data & ea_alterable));                                                                      \
    CC_Z = !(dst & mask);                                                                                              \
    STORE_EA(size, operation);

#define BIT_OP_TEST(size, bit, mode)                                                                                   \
    VALID_EA(mode);                                                                                                    \
    UINT(size) mask = 1u << ((bit) & ((size) - 1)); /* bit < size even if a caller forgets to mask */                  \
    LOAD_EA_WITH_UPDATE(size, dst);                                                                                    \
    CC_Z = !(dst & mask);

// MOVE src,dst: snapshot source An and roll back on any bus error so the
// Format-$B RTE retry restarts with pre-instruction An values.  Without this,
// a dest-side page fault on (An)+,(An)+ leaks +size into the source An on
// retry — seen in A/UX libc1_s memcpy crossing virgin user pages.
#define MOVE(size, src_modes)                                                                                          \
    VALID_EA(src_modes);                                                                                               \
    uint32_t move_src_an_save_ = (EA_MODE == 3 || EA_MODE == 4) ? cpu->a[EA_REG] : 0;                                  \
    LOAD_EA_WITH_UPDATE(size, src);                                                                                    \
    WRITE_EA(size, opcode >> 6 & 7, opcode >> 9 & 7, src);                                                             \
    if (__builtin_expect(g_bus_error_pending, 0) && (EA_MODE == 3 || EA_MODE == 4))                                    \
        cpu->a[EA_REG] = move_src_an_save_;                                                                            \
    UPDATE_NZ_CLEAR_CV(src);

#define MOVEA(size)                                                                                                    \
    VALID_EA(ea_any);                                                                                                  \
    LOAD_EA_WITH_UPDATE(size, src);                                                                                    \
    AN = (int32_t)(int##size##_t)src;

#define CLR(size)                                                                                                      \
    STORE_EA(size, 0);                                                                                                 \
    CC_N = CC_V = CC_C = 0;                                                                                            \
    CC_Z = 1;

#define DBCC_DN_LABEL                                                                                                  \
    if (CC)                                                                                                            \
        PC += 2;                                                                                                       \
    else {                                                                                                             \
        /* DBcc counts in the low word of Dn only */                                                                   \
        int16_t counter = (int16_t)((uint16_t)DY - 1);                                                                 \
        STORE_DN(16, EA_REG, counter);                                                                                 \
        PC += counter == -1 ? 2 : (int32_t)(int16_t)FETCH16_NO_INC();                                                  \
    }

#define NOT(size)                                                                                                      \
    LOAD_EA(size, dst, (ea_data & ea_alterable));                                                                      \
    dst = ~dst;                                                                                                        \
    UPDATE_NZ_CLEAR_CV(dst);                                                                                           \
    STORE_EA(size, dst);

#define GET_EA       CALCULATE_EA(4, EA_MODE, EA_REG, true)
#define EA_D16_AN(x) uint32_t x = CALCULATE_EA(2, 5, EA_REG, true)

// Push a longword onto the stack with bus-error retry safety.
//
// A naive `SP -= 4; WRITE32(SP, x)` leaks the SP decrement on a write fault
// retry: the kernel's Format-$B frame restarts the instruction, but SP has
// already moved, so the retried push decrements SP a second time and lands
// the value 4 bytes lower than intended.  Symptom seen in A/UX 3.0.1 user
// code crossing a virgin user-stack page: BSR/JSR/PEA on the boundary saves
// its longword 4 bytes off, and the matching RTS/MOVEM-pop later pops from
// the wrong slot — random data interpreted as a return PC → vector-4 SIGILL.
//
// Fix: write to (SP-4) first; only commit the SP update if the write didn't
// raise a deferred bus error.  Sibling of the MOVEM (65d3ff4), write_ea
// (959728c), and MOVE src-An (82fb501) restart-safety fixes.
#define PUSH(x)                                                                                                        \
    WRITE32(SP - 4, (x));                                                                                              \
    if (__builtin_expect(!g_bus_error_pending, 1))                                                                     \
        SP -= 4;

#define POP16(x)                                                                                                       \
    x = READ16(SP);                                                                                                    \
    SP += 2;
#define POP32(x)                                                                                                       \
    x = READ32(SP);                                                                                                    \
    SP += 4;

#define OP(x)                                                                                                          \
    { x; }

#define SUPER(x)                                                                                                       \
    if (IS_SUPERVISOR()) {                                                                                             \
        x;                                                                                                             \
    } else {                                                                                                           \
        EXC_PRIVILEGE();                                                                                               \
    }

#define OP_MOVE_EA_SR       OP(VALID_EA(ea_data); SUPER(LOAD_EA_WITH_UPDATE(16, s); SET_SR(s)))
#define OP_BCHG_L_DX_DY     OP(BIT_OP_WRITE(32, DX & 0x1F, dst ^ mask))
#define OP_BCHG_B_DN_EA     OP(BIT_OP_WRITE(8, DX & 7, dst ^ mask))
#define OP_BCHG_L_DATA_DN   OP(BIT_OP_WRITE(32, FETCH16() & 0x1F, dst ^ mask))
#define OP_BCHG_B_DATA_EA   OP(BIT_OP_WRITE(8, FETCH16() & 7, dst ^ mask))
#define OP_BCLR_L_DX_DY     OP(BIT_OP_WRITE(32, DX & 0x1F, dst & ~mask))
#define OP_BCLR_B_DN_EA     OP(BIT_OP_WRITE(8, DX & 7, dst & ~mask))
#define OP_BCLR_L_DATA_DN   OP(BIT_OP_WRITE(32, FETCH16() & 0x1F, dst & ~mask))
#define OP_BCLR_B_DATA_EA   OP(BIT_OP_WRITE(8, FETCH16() & 7, dst & ~mask))
#define OP_BSET_L_DX_DY     OP(BIT_OP_WRITE(32, DX & 0x1F, dst | mask))
#define OP_BSET_B_DN_EA     OP(BIT_OP_WRITE(8, DX & 7, dst | mask))
#define OP_BSET_L_DATA_DN   OP(BIT_OP_WRITE(32, FETCH16() & 0x1F, dst | mask))
#define OP_BSET_B_DATA_EA   OP(BIT_OP_WRITE(8, FETCH16() & 7, dst | mask))
#define OP_BTST_L_DX_DY     OP(BIT_OP_TEST(32, DX & 0x1F, ea_data - ea_xxx))
#define OP_BTST_B_DN_EA     OP(BIT_OP_TEST(8, DX & 7, ea_data))
#define OP_BTST_L_DATA_DN   OP(BIT_OP_TEST(32, FETCH16() & 0x1F, ea_data - ea_xxx))
#define OP_BTST_B_DATA_EA   OP(BIT_OP_TEST(8, FETCH16() & 7, ea_data - ea_xxx))
#define OP_MOVE_B_EA_EA     OP(VALID_EA_MOVE((ea_data & ea_alterable)); MOVE(8, ea_any - ea_an))
#define OP_MOVE_W_EA_EA     OP(VALID_EA_MOVE((ea_data & ea_alterable)); MOVE(16, ea_any))
#define OP_MOVE_L_EA_EA     OP(VALID_EA_MOVE((ea_data & ea_alterable)); MOVE(32, ea_any))
#define OP_MOVEA_W_EA_AN    OP(MOVEA(16))
#define OP_MOVEA_L_EA_AN    OP(MOVEA(32))
#define OP_CLR_B_EA         OP(VALID_EA((ea_data & ea_alterable)); CLR(8))
#define OP_CLR_W_EA         OP(VALID_EA((ea_data & ea_alterable)); CLR(16))
#define OP_CLR_L_EA         OP(VALID_EA((ea_data & ea_alterable)); CLR(32))
#define OP_NOT_B_EA         OP(NOT(8))
#define OP_NOT_W_EA         OP(NOT(16))
#define OP_NOT_L_EA         OP(NOT(32))
#define OP_SWAP_DN          OP(DY = (DY >> 16) | (DY << 16); UPDATE_NZ_CLEAR_CV(DY))
#define OP_JSR_EA           OP(VALID_EA(ea_control); uint32_t ea = GET_EA; PUSH(PC); PC = ea)
#define OP_JMP_EA           OP(VALID_EA(ea_control); PC = GET_EA)
#define OP_MOVEP_W_DX_D16AY OP(EA_D16_AN(ea); WRITE2x8(ea, DX))
#define OP_MOVEP_L_D16AY_DX OP(EA_D16_AN(ea); DX = READ4x8(ea))
#define OP_MOVEP_W_D16AY_DX OP(EA_D16_AN(ea); STORE_DN(16, opcode >> 9 & 7, READ2x8(ea)))
#define OP_MOVEP_L_DX_D16AY OP(EA_D16_AN(ea); WRITE4x8(ea, DX))
#define OP_PEA_EA           OP(VALID_EA(ea_control); uint32_t ea = GET_EA; PUSH(ea))
#define OP_BSR_B_LABEL      OP(uint32_t p = PC + DISP8(); PUSH(PC); PC = p)
#define OP_BSR_W_LABEL      OP(uint32_t p = PC + DISP16(); PUSH(PC); PC = p)
#define OP_BSR_L_LABEL      OP(uint32_t p = PC + DISP32(); PUSH(PC); PC = p)
#define OP_ORI_B_DATA_CCR   OP(TO_CCR(|))
#define OP_ORI_W_DATA_SR    OP(TO_SR(|))
#define OP_ORI_B_DATA_EA    OP(IMM_LOGICAL(8, |))
#define OP_ORI_W_DATA_EA    OP(IMM_LOGICAL(16, |))
#define OP_ORI_L_DATA_EA    OP(IMM_LOGICAL(32, |))
#define OP_ANDI_B_DATA_CCR  OP(TO_CCR(&))
#define OP_ANDI_W_DATA_SR   OP(TO_SR(&))
#define OP_ANDI_B_DATA_EA   OP(IMM_LOGICAL(8, &))
#define OP_ANDI_W_DATA_EA   OP(IMM_LOGICAL(16, &))
#define OP_ANDI_L_DATA_EA   OP(IMM_LOGICAL(32, &))
#define OP_RTM_RN           OP_UNDEFINED
#define OP_CALLM_DATA_EA    OP_UNDEFINED
#define OP_SUBI_B_DATA_EA   OP(SUBI(8))
#define OP_SUBI_W_DATA_EA   OP(SUBI(16))
#define OP_SUBI_L_DATA_EA   OP(SUBI(32))
#define OP_ADDI_B_DATA_EA   OP(ADDI(8))
#define OP_ADDI_W_DATA_EA   OP(ADDI(16))
#define OP_ADDI_L_DATA_EA   OP(ADDI(32))
#define OP_EORI_B_DATA_CCR  OP(TO_CCR(^))
#define OP_EORI_W_DATA_SR   OP(TO_SR(^))
#define OP_EORI_B_DATA_EA   OP(IMM_LOGICAL(8, ^))
#define OP_EORI_W_DATA_EA   OP(IMM_LOGICAL(16, ^))
#define OP_EORI_L_DATA_EA   OP(IMM_LOGICAL(32, ^))
#define OP_CMPI_B_DATA_EA   OP(CMPI(8))
#define OP_CMPI_W_DATA_EA   OP(CMPI(16))
#define OP_CMPI_L_DATA_EA   OP(CMPI(32))
#define OP_NEGX_B_EA        OP(NEGX(8))
#define OP_NEGX_W_EA        OP(NEGX(16))
#define OP_NEGX_L_EA        OP(NEGX(32))
#define OP_NEG_B_EA         OP(NEG(8))
#define OP_NEG_W_EA         OP(NEG(16))
#define OP_NEG_L_EA         OP(NEG(32))
#define OP_MOVE_B_EA_CCR    OP(VALID_EA(ea_data); LOAD_EA_WITH_UPDATE(16, src); WRITE_CCR(src))
#define OP_NBCD_B_EA        OP(LOAD_EA(8, src, (ea_data & ea_alterable)); STORE_EA(8, SBCD(0, src)))
#define OP_EXT_W_DN         OP(UINT(16) r = S_EXT_8TO16(DY); STORE_DN(16, EA_REG, r); UPDATE_NZ_CLEAR_CV(r))
#define OP_EXT_L_DN         OP(UINT(32) r = S_EXT_16TO32(DY); DY = r; UPDATE_NZ_CLEAR_CV(r))
#define OP_ILLEGAL          OP_UNDEFINED
#define OP_TAS_B_EA         OP(LOAD_EA(8, ea, (ea_data & ea_alterable)); UPDATE_NZ_CLEAR_CV(ea); ea |= 0x80; STORE_EA(8, ea))
#define OP_MOVEM_W_EA_LIST  OP(VALID_EA(ea_control + ea_an_plus); MOVEM_TO_REGISTER(opcode, 16))
#define OP_MOVEM_L_EA_LIST  OP(VALID_EA(ea_control + ea_an_plus); MOVEM_TO_REGISTER(opcode, 32))
#define OP_TRAP_VECTOR      OP(EXC_TRAP(opcode & 0xF))
// RESET asserts the bus /RESET line → reset external peripherals (SCSI, NuBus
// cards) to power-on; the CPU core (registers/caches/MMU) is left untouched.
// The Mac warm-restart ROM path relies on this (see system_reset_devices).
#define OP_RESET OP(SUPER(system_reset_devices()))
// STOP: load SR and halt instruction fetch until an interrupt.  Set the stopped
// flag AND drain the sprint budget (*instructions = 0) so the decoder loop exits
// immediately — otherwise it would keep executing the instructions *after* the
// STOP (e.g. fall through past the Lisa scheduler's Pause), corrupting state.
// SET_SR runs cpu_check_interrupt last, so an already-pending interrupt clears
// `stopped` and is taken normally on the next sprint.
// STOP: load SR from the immediate, then halt until an interrupt.
//
// MC68030UM 8.1.7 / MC68040UM 8.2.6: "A STOP instruction that begins execution
// with T1 = 1 and T0 = 0 forces a trace exception after it loads the status
// register.  Upon return from the trace handler routine, execution continues
// with the instruction following the STOP, and THE PROCESSOR NEVER ENTERS THE
// STOPPED CONDITION."  So the stop is suppressed by the T1 state the
// instruction STARTED with -- read before SET_SR overwrites it -- not by the
// value the immediate loads.
#define OP_STOP_DATA                                                                                                   \
    OP(SUPER({                                                                                                         \
        uint16_t sr = FETCH16();                                                                                       \
        bool traced_ = (cpu->trace & 2) != 0;                                                                          \
        if (!traced_)                                                                                                  \
            cpu->stopped = 1;                                                                                          \
        memory_end_sprint(instructions);                                                                               \
        SET_SR(sr);                                                                                                    \
    }))
#define OP_RTS   OP(POP32(PC))
#define OP_TRAPV OP(if (CC_V) EXC_TRAPV())
#define OP_RTR   OP(uint16_t ccr; POP16(ccr); WRITE_CCR(ccr); POP32(PC))
// LINK: fetch the displacement word *before* mutating any register, so a
// page-cross fault on the immediate restarts the instruction cleanly (the
// pre-PUSH/AY-update state is untouched). M68000PRM §8.1: An is pushed,
// then An <- SP, then SP += disp.
#define OP_LINK                                                                                                        \
    OP({                                                                                                               \
        int32_t disp_ = (int32_t)(int16_t)FETCH16();                                                                   \
        uint32_t ay_ = AY;                                                                                             \
        PUSH(ay_);                                                                                                     \
        AY = SP;                                                                                                       \
        SP += disp_;                                                                                                   \
    })
#define OP_UNLK               OP(SP = AY; uint32_t a; POP32(a); AY = a)
#define OP_NOP                OP(/* no-op */)
#define OP_MOVE_AN_USP        OP(SUPER(SET_USP(A(EA_REG))))
#define OP_MOVE_USP_AN        OP(SUPER(A(EA_REG) = GET_USP()))
#define OP_SCC_EA             OP(VALID_EA((ea_data & ea_alterable)); STORE_EA(8, CC ? 0xFF : 0))
#define OP_DBCC_DN_LABEL      OP(DBCC_DN_LABEL)
#define OP_SUBQ_W_DATA_AN     OP(AY -= DATA)
#define OP_SUBQ_L_DATA_AN     OP(AY -= DATA)
#define OP_SUBQ_B_DATA_EA     OP(SUBQ(8))
#define OP_SUBQ_W_DATA_EA     OP(SUBQ(16))
#define OP_SUBQ_L_DATA_EA     OP(SUBQ(32))
#define OP_ADDQ_W_DATA_AN     OP(AY += DATA)
#define OP_ADDQ_L_DATA_AN     OP(AY += DATA)
#define OP_ADDQ_B_DATA_EA     OP(ADDQ(8))
#define OP_ADDQ_W_DATA_EA     OP(ADDQ(16))
#define OP_ADDQ_L_DATA_EA     OP(ADDQ(32))
#define OP_BCC_L_DISPLACEMENT OP(PC += CC ? (int32_t)FETCH32_NO_INC() : 4)
#define OP_BCC_W_DISPLACEMENT OP(PC += CC ? (int32_t)(int16_t)FETCH16_NO_INC() : 2)
#define OP_BCC_B_DISPLACEMENT OP(PC += CC ? (int32_t)(int8_t)(opcode & 0xFF) : 0)
#define OP_MOVEQ_L_DATA_DN    OP(uint32_t res = (int32_t)(int8_t)opcode; DX = res; UPDATE_NZ_CLEAR_CV(res))
#define OP_SBCD_DX_DY         OP(XBCD_DY_DX(SBCD))
#define OP_SBCD_AX_AY         OP(XBCD_AY_AX(SBCD))
#define OP_OR_B_EA_DN         OP(LOGICAL_EA_DN(8, |, ea_data))
#define OP_OR_W_EA_DN         OP(LOGICAL_EA_DN(16, |, ea_data))
#define OP_OR_L_EA_DN         OP(LOGICAL_EA_DN(32, |, ea_data))
#define OP_OR_B_DN_EA         OP(LOGICAL_DN_EA(8, |, (ea_memory & ea_alterable)))
#define OP_OR_W_DN_EA         OP(LOGICAL_DN_EA(16, |, (ea_memory & ea_alterable)))
#define OP_OR_L_DN_EA         OP(LOGICAL_DN_EA(32, |, (ea_memory & ea_alterable)))
#define OP_DIVU_W_EA_DN       OP(DIV16U)
#define OP_DIVS_W_EA_DN       OP(DIV16S)
#define OP_SUBA_W_EA_AN       OP(VALID_EA(ea_any); LOAD_EA_WITH_UPDATE(16, src); AN -= (int32_t)(int16_t)src)
#define OP_SUBA_L_EA_AN       OP(VALID_EA(ea_any); LOAD_EA_WITH_UPDATE(32, src); AN -= src)
#define OP_SUBX_B_DX_DY       OP(SUBX_DX_DY(8))
#define OP_SUBX_W_DX_DY       OP(SUBX_DX_DY(16))
#define OP_SUBX_L_DX_DY       OP(SUBX_DX_DY(32))
#define OP_SUBX_B_AX_AY       OP(SUBX_AX_AY(8))
#define OP_SUBX_W_AX_AY       OP(SUBX_AX_AY(16))
#define OP_SUBX_L_AX_AY       OP(SUBX_AX_AY(32))
#define OP_SUB_B_EA_DN        OP(SUB_EA_DN(8, ea_any - ea_an))
#define OP_SUB_W_EA_DN        OP(SUB_EA_DN(16, ea_any))
#define OP_SUB_L_EA_DN        OP(SUB_EA_DN(32, ea_any))
#define OP_SUB_B_DN_EA        OP(SUB_DN_EA(8))
#define OP_SUB_W_DN_EA        OP(SUB_DN_EA(16))
#define OP_SUB_L_DN_EA        OP(SUB_DN_EA(32))
#define OP_ATRAP              OP(EXC_ATRAP())
#define OP_CMPM_B_AY_AX       OP(CMPM_AY_AX(8))
#define OP_CMPM_W_AY_AX       OP(CMPM_AY_AX(16))
#define OP_CMPM_L_AY_AX       OP(CMPM_AY_AX(32))
#define OP_CMPA_W_EA_AN       OP(CMPA_EA_AN(16))
#define OP_CMPA_L_EA_AN       OP(CMPA_EA_AN(32))
#define OP_EOR_B_DN_EA        OP(LOGICAL_DN_EA(8, ^, (ea_data & ea_alterable)))
#define OP_EOR_W_DN_EA        OP(LOGICAL_DN_EA(16, ^, (ea_data & ea_alterable)))
#define OP_EOR_L_DN_EA        OP(LOGICAL_DN_EA(32, ^, (ea_data & ea_alterable)))
#define OP_CMP_B_EA_DN        OP(CMP_EA_DN(8, ea_any - ea_an))
#define OP_CMP_W_EA_DN        OP(CMP_EA_DN(16, ea_any))
#define OP_CMP_L_EA_DN        OP(CMP_EA_DN(32, ea_any))
#define OP_AND_B_EA_DN        OP(LOGICAL_EA_DN(8, &, ea_data))
#define OP_AND_W_EA_DN        OP(LOGICAL_EA_DN(16, &, ea_data))
#define OP_AND_L_EA_DN        OP(LOGICAL_EA_DN(32, &, ea_data))
#define OP_MULU_W_EA_DN       OP(MUL_W(uint))
#define OP_ABCD_DY_DX         OP(XBCD_DY_DX(ABCD))
#define OP_ABCD_AY_AX         OP(XBCD_AY_AX(ABCD))
#define OP_AND_B_DN_EA        OP(LOGICAL_DN_EA(8, &, (ea_memory & ea_alterable)))
#define OP_EXG_DX_DY          OP(EXG(DX, DY))
#define OP_EXG_AX_AY          OP(EXG(AX, AY))
#define OP_AND_W_DN_EA        OP(LOGICAL_DN_EA(16, &, (ea_memory & ea_alterable)))
#define OP_EXG_DX_AY          OP(EXG(DX, AY))
#define OP_AND_L_DN_EA        OP(LOGICAL_DN_EA(32, &, (ea_memory & ea_alterable)))
#define OP_MULS_W_EA_DN       OP(MUL_W(int))
#define OP_ADD_B_EA_DN        OP(ADD_EA_DN(8, ea_any - ea_an))
#define OP_ADD_W_EA_DN        OP(ADD_EA_DN(16, ea_any))
#define OP_ADD_L_EA_DN        OP(ADD_EA_DN(32, ea_any))
#define OP_ADDA_W_EA_AN       OP(VALID_EA(ea_any); LOAD_EA_WITH_UPDATE(16, src); AN += (int32_t)(int16_t)src)
#define OP_ADDX_B_DY_DX       OP(ADDX_DX_DY(8))
#define OP_ADDX_B_AY_AX       OP(ADDX_AX_AY(8))
#define OP_ADD_B_DN_EA        OP(ADD_DN_EA(8))
#define OP_ADDX_W_DY_DX       OP(ADDX_DX_DY(16))
#define OP_ADDX_W_AY_AX       OP(ADDX_AX_AY(16))
#define OP_ADD_W_DN_EA        OP(ADD_DN_EA(16))
#define OP_ADDX_L_DY_DX       OP(ADDX_DX_DY(32))
#define OP_ADDX_L_AY_AX       OP(ADDX_AX_AY(32))
#define OP_ADD_L_DN_EA        OP(ADD_DN_EA(32))
#define OP_ADDA_L_EA_AN       OP(VALID_EA(ea_any); LOAD_EA_WITH_UPDATE(32, src); AN += src)
#define OP_ASR_B_DATA_DY      OP(SHIFT_RIGHT(8, DY, DATA, (int8_t)d >> MIN(c, 7)))
#define OP_LSR_B_DATA_DY      OP(SHIFT_RIGHT(8, DY, DATA, c > 7 ? 0 : d >> c))
#define OP_ROXR_B_DATA_DY     OP(ROXR(8, DY, DATA))
#define OP_ROR_B_DATA_DY      OP(ROTATE_RIGHT(8, DY, DATA))
#define OP_ASR_B_DX_DY        OP(SHIFT_RIGHT(8, DY, DX & 0x3F, (int8_t)d >> MIN(c, 7)))
#define OP_LSR_B_DX_DY        OP(SHIFT_RIGHT(8, DY, DX & 0x3F, c > 7 ? 0 : d >> c))
#define OP_ROXR_B_DX_DY       OP(ROXR(8, DY, (DX & 0x3F) % 9))
#define OP_ROR_B_DX_DY        OP(ROTATE_RIGHT(8, DY, DX & 0x3F))
#define OP_ASR_W_DATA_DY      OP(SHIFT_RIGHT(16, DY, DATA, (int16_t)d >> c))
#define OP_LSR_W_DATA_DY      OP(SHIFT_RIGHT(16, DY, DATA, d >> c))
#define OP_ROXR_W_DATA_DY     OP(ROXR(16, DY, DATA))
#define OP_ROR_W_DATA_DY      OP(ROTATE_RIGHT(16, DY, DATA))
#define OP_ASR_W_DX_DY        OP(SHIFT_RIGHT(16, DY, DX & 0x3F, (int16_t)d >> MIN(c, 15)))
#define OP_LSR_W_DX_DY        OP(SHIFT_RIGHT(16, DY, DX & 0x3F, c > 15 ? 0 : d >> c))
#define OP_ROXR_W_DX_DY       OP(ROXR(16, DY, (DX & 0x3F) % 17))
#define OP_ROR_W_DX_DY        OP(ROTATE_RIGHT(16, DY, DX & 0x3F))
#define OP_ASR_L_DATA_DY      OP(SHIFT_RIGHT(32, DY, DATA, (int32_t)d >> c))
#define OP_LSR_L_DATA_DY      OP(SHIFT_RIGHT(32, DY, DATA, d >> c))
#define OP_ROXR_L_DATA_DY     OP(ROXR(32, DY, DATA))
#define OP_ROR_L_DATA_DY      OP(ROTATE_RIGHT(32, DY, DATA))
#define OP_ASR_L_DX_DY        OP(SHIFT_RIGHT(32, DY, DX & 0x3F, (int32_t)d >> MIN(c, 31)))
#define OP_LSR_L_DX_DY        OP(SHIFT_RIGHT(32, DY, DX & 0x3F, c > 31 ? 0 : d >> c))
#define OP_ROXR_L_DX_DY       OP(ROXR(32, DY, (DX & 0x3F) % 33))
#define OP_ROR_L_DX_DY        OP(ROTATE_RIGHT(32, DY, DX & 0x3F))
#define OP_ASR_W_EA           OP(SHIFT_EA_R((int16_t)ea >> 1); CC_X = ea & 1)
#define OP_LSR_W_EA           OP(SHIFT_EA_R(ea >> 1); CLEAR_N(); CC_X = ea & 1)
#define OP_ROXR_W_EA          OP(SHIFT_EA_R(ea >> 1 | (CC_X ? 0x8000 : 0)); CC_X = ea & 1)
#define OP_ROR_W_EA           OP(SHIFT_EA_R(ea >> 1 | ea << 15))
#define OP_ASL_B_DATA_DY      OP(ASHIFT_LEFT(8, DY, DATA, c < 8 ? d << c : 0))
#define OP_LSL_B_DATA_DY      OP(LSHIFT_LEFT(8, DY, DATA, c < 8 ? d << c : 0))
#define OP_ROXL_B_DATA_DY     OP(ROXL(8, DY, DATA))
#define OP_ROL_B_DATA_DY      OP(ROTATE_LEFT(8, DY, DATA))
#define OP_ASL_B_DX_DY        OP(ASHIFT_LEFT(8, DY, DX & 0x3F, c < 8 ? d << c : 0))
#define OP_LSL_B_DX_DY        OP(LSHIFT_LEFT(8, DY, DX & 0x3F, c < 8 ? d << c : 0))
#define OP_ROXL_B_DX_DY       OP(ROXL(8, DY, (DX & 0x3F) % 9))
#define OP_ROL_B_DX_DY        OP(ROTATE_LEFT(8, DY, DX & 0x3F))
#define OP_ASL_W_DATA_DY      OP(ASHIFT_LEFT(16, DY, DATA, d << c))
#define OP_LSL_W_DATA_DY      OP(LSHIFT_LEFT(16, DY, DATA, d << c))
#define OP_ROXL_W_DATA_DY     OP(ROXL(16, DY, DATA))
#define OP_ROL_W_DATA_DY      OP(ROTATE_LEFT(16, DY, DATA))
#define OP_ASL_W_DX_DY        OP(ASHIFT_LEFT(16, DY, DX & 0x3F, c < 16 ? d << c : 0))
#define OP_LSL_W_DX_DY        OP(LSHIFT_LEFT(16, DY, DX & 0x3F, c < 16 ? d << c : 0))
#define OP_ROXL_W_DX_DY       OP(ROXL(16, DY, (DX & 0x3F) % 17))
#define OP_ROL_W_DX_DY        OP(ROTATE_LEFT(16, DY, DX & 0x3F))
#define OP_ASL_L_DATA_DY      OP(ASHIFT_LEFT(32, DY, DATA, d << c))
#define OP_LSL_L_DATA_DY      OP(LSHIFT_LEFT(32, DY, DATA, d << c))
#define OP_ROXL_L_DATA_DY     OP(ROXL(32, DY, DATA))
#define OP_ROL_L_DATA_DY      OP(ROTATE_LEFT(32, DY, DATA))
#define OP_ASL_L_DX_DY        OP(ASHIFT_LEFT(32, DY, DX & 0x3F, c < 32 ? d << c : 0))
#define OP_LSL_L_DX_DY        OP(LSHIFT_LEFT(32, DY, DX & 0x3F, c < 32 ? d << c : 0))
#define OP_ROXL_L_DX_DY       OP(ROXL(32, DY, (DX & 0x3F) % 33))
#define OP_ROL_L_DX_DY        OP(ROTATE_LEFT(32, DY, DX & 0x3F))
#define OP_ASL_W_EA           OP(SHIFT_EA_L(ea << 1); CC_X = CC_C; CC_V = (ea ^ ea << 1) & 0x8000)
#define OP_LSL_W_EA           OP(SHIFT_EA_L(ea << 1); CC_X = CC_C)
#define OP_ROXL_W_EA          OP(SHIFT_EA_L(ea << 1 | (CC_X ? 1 : 0)); CC_X = CC_C)
#define OP_ROL_W_EA           OP(SHIFT_EA_L(ea << 1 | ea >> 15))
// The 68040's own line-F instructions.  A 68030 has none of them, so the
// line-F exception is the correct behaviour here; cpu_68040.c overrides them.
#define OP_CINVL_CACHES_AN   OP(EXC_FTRAP())
#define OP_CINVP_CACHES_AN   OP(EXC_FTRAP())
#define OP_CINVA_CACHES      OP(EXC_FTRAP())
#define OP_CPUSHL_CACHES_AN  OP(EXC_FTRAP())
#define OP_CPUSHP_CACHES_AN  OP(EXC_FTRAP())
#define OP_CPUSHA_CACHES     OP(EXC_FTRAP())
#define OP_PFLUSH_AN         OP(EXC_FTRAP())
#define OP_PFLUSHN_AN        OP(EXC_FTRAP())
#define OP_PFLUSHA           OP(EXC_FTRAP())
#define OP_PFLUSHAN          OP(EXC_FTRAP())
#define OP_PTESTR_AN         OP(EXC_FTRAP())
#define OP_PTESTW_AN         OP(EXC_FTRAP())
#define OP_MOVE16_AN_P_XXX_L OP(EXC_FTRAP())
#define OP_MOVE16_XXX_L_AN_P OP(EXC_FTRAP())
#define OP_MOVE16_AN_XXX_L   OP(EXC_FTRAP())
#define OP_MOVE16_XXX_L_AN   OP(EXC_FTRAP())
#define OP_MOVE16_AN_P_AN_P  OP(EXC_FTRAP())

// ============================================================
// CPU-specific instruction definitions
// ============================================================
#ifdef CPU_DECODER_IS_68030

// OP_UNDEFINED: the saved PC is the instruction's own address (MC68030UM 8.1.5).
#define OP_UNDEFINED OP(exception(cpu, 0x010, cpu->instruction_pc, cpu_get_sr(cpu)); continue)

// --- Bit-field helper functions (register and memory operands) ---

// Extract bit field from a 32-bit register value.
// Offset is treated modulo 32; field occupies bits (31-off)..(31-off-w+1).
static inline uint32_t bf_extract_reg(uint32_t val, int32_t offset, uint32_t w) {
    uint32_t o = (uint32_t)offset & 31u;
    uint32_t rotated = o ? ((val << o) | (val >> (32u - o))) : val;
    uint32_t mask = (w == 32u) ? 0xFFFFFFFFu : ((1u << w) - 1u);
    return (rotated >> (32u - w)) & mask;
}

// Insert value into bit field of a 32-bit register.
static inline uint32_t bf_insert_reg(uint32_t dst, int32_t offset, uint32_t w, uint32_t src) {
    uint32_t o = (uint32_t)offset & 31u;
    uint32_t mask = (w == 32u) ? 0xFFFFFFFFu : ((1u << w) - 1u);
    uint32_t at_msb = (src & mask) << (32u - w); // value at MSB position
    uint32_t pos_val = o ? ((at_msb >> o) | (at_msb << (32u - o))) : at_msb;
    uint32_t field_mask = mask << (32u - w);
    uint32_t pos_mask = o ? ((field_mask >> o) | (field_mask << (32u - o))) : field_mask;
    return (dst & ~pos_mask) | (pos_val & pos_mask);
}

// Extract bit field from memory using READ8 macro.
// bf_offset may be negative (or large); ea is the base effective address.
#define BF_EXTRACT_MEM(ea, bf_offset, w, result)                                                                       \
    do {                                                                                                               \
        int32_t bfx_bo_ = (bf_offset);                                                                                 \
        uint32_t bfx_w_ = (uint32_t)(w);                                                                               \
        uint32_t bfx_ea_ = (uint32_t)(ea);                                                                             \
        /* Adjust for negative offset */                                                                               \
        if (bfx_bo_ < 0) {                                                                                             \
            int32_t bfx_adj_ = ((-bfx_bo_ + 7) >> 3) << 3;                                                             \
            bfx_ea_ -= (uint32_t)(bfx_adj_ >> 3);                                                                      \
            bfx_bo_ += bfx_adj_;                                                                                       \
        }                                                                                                              \
        uint32_t bfx_byte_off_ = (uint32_t)bfx_bo_ >> 3;                                                               \
        uint32_t bfx_bit_in_ = (uint32_t)bfx_bo_ & 7u;                                                                 \
        uint32_t bfx_n_ = (bfx_bit_in_ + bfx_w_ + 7u) >> 3;                                                            \
        uint64_t bfx_data_ = 0;                                                                                        \
        for (uint32_t bfx_i_ = 0; bfx_i_ < bfx_n_ && bfx_i_ < 5; bfx_i_++)                                             \
            bfx_data_ = (bfx_data_ << 8) | (uint8_t)READ8(bfx_ea_ + bfx_byte_off_ + bfx_i_);                           \
        uint32_t bfx_shift_ = bfx_n_ * 8u - bfx_bit_in_ - bfx_w_;                                                      \
        uint32_t bfx_mask_ = (bfx_w_ == 32u) ? 0xFFFFFFFFu : ((1u << bfx_w_) - 1u);                                    \
        (result) = (uint32_t)((bfx_data_ >> bfx_shift_) & bfx_mask_);                                                  \
    } while (0)

// Write bit field to memory using WRITE8 macro.
#define BF_INSERT_MEM(ea, bf_offset, w, value)                                                                         \
    do {                                                                                                               \
        int32_t bfi_bo_ = (bf_offset);                                                                                 \
        uint32_t bfi_w_ = (uint32_t)(w);                                                                               \
        uint32_t bfi_ea_ = (uint32_t)(ea);                                                                             \
        uint32_t bfi_val_ = (uint32_t)(value);                                                                         \
        if (bfi_bo_ < 0) {                                                                                             \
            int32_t bfi_adj_ = ((-bfi_bo_ + 7) >> 3) << 3;                                                             \
            bfi_ea_ -= (uint32_t)(bfi_adj_ >> 3);                                                                      \
            bfi_bo_ += bfi_adj_;                                                                                       \
        }                                                                                                              \
        uint32_t bfi_byte_off_ = (uint32_t)bfi_bo_ >> 3;                                                               \
        uint32_t bfi_bit_in_ = (uint32_t)bfi_bo_ & 7u;                                                                 \
        uint32_t bfi_n_ = (bfi_bit_in_ + bfi_w_ + 7u) >> 3;                                                            \
        uint8_t bfi_bytes_[5] = {0};                                                                                   \
        for (uint32_t bfi_i_ = 0; bfi_i_ < bfi_n_ && bfi_i_ < 5; bfi_i_++)                                             \
            bfi_bytes_[bfi_i_] = (uint8_t)READ8(bfi_ea_ + bfi_byte_off_ + bfi_i_);                                     \
        uint64_t bfi_data_ = 0;                                                                                        \
        for (uint32_t bfi_i_ = 0; bfi_i_ < bfi_n_ && bfi_i_ < 5; bfi_i_++)                                             \
            bfi_data_ = (bfi_data_ << 8) | bfi_bytes_[bfi_i_];                                                         \
        uint32_t bfi_mask_ = (bfi_w_ == 32u) ? 0xFFFFFFFFu : ((1u << bfi_w_) - 1u);                                    \
        uint32_t bfi_shift_ = bfi_n_ * 8u - bfi_bit_in_ - bfi_w_;                                                      \
        bfi_data_ =                                                                                                    \
            (bfi_data_ & ~((uint64_t)bfi_mask_ << bfi_shift_)) | ((uint64_t)(bfi_val_ & bfi_mask_) << bfi_shift_);     \
        for (uint32_t bfi_i_ = 0; bfi_i_ < bfi_n_ && bfi_i_ < 5; bfi_i_++)                                             \
            WRITE8(bfi_ea_ + bfi_byte_off_ + bfi_i_, (uint8_t)(bfi_data_ >> ((bfi_n_ - 1u - bfi_i_) * 8u)));           \
    } while (0)

// Decode bit-field extension word: returns offset and width; sets CC_N from MSB.
// Uses FETCH16() which advances PC.
#define BF_DECODE_EXT(bf_off, bf_w)                                                                                    \
    do {                                                                                                               \
        uint16_t ext_ = FETCH16();                                                                                     \
        (bf_off) = (ext_ & 0x0800) ? (int32_t)D((ext_ >> 6) & 7) : (int32_t)((ext_ >> 6) & 31u);                       \
        (bf_w) = (ext_ & 0x0020) ? (D(ext_ & 7) & 31u) : (uint32_t)(ext_ & 31u);                                       \
        if ((bf_w) == 0u)                                                                                              \
            (bf_w) = 32u;                                                                                              \
    } while (0)

// Bit field CC update: N = MSB of field, Z = (field == 0), V=C=0
#define BF_UPDATE_CC(field, w)                                                                                         \
    do {                                                                                                               \
        CC_N = ((field) >> ((w) - 1u)) & 1u;                                                                           \
        CC_Z = ((field) == 0u);                                                                                        \
        CC_V = CC_C = 0;                                                                                               \
    } while (0)

// Decode bit-field extension word including Dn destination (for BFEXTU/BFEXTS/BFFFO/BFINS).
#define BF_DECODE_EXT_WITH_DN(off_, w_, dn_)                                                                           \
    do {                                                                                                               \
        uint16_t bfd_ext_ = FETCH16();                                                                                 \
        (off_) = (bfd_ext_ & 0x0800) ? (int32_t)D((bfd_ext_ >> 6) & 7) : (int32_t)((bfd_ext_ >> 6) & 31u);             \
        (w_) = (bfd_ext_ & 0x0020) ? (D(bfd_ext_ & 7) & 31u) : (uint32_t)(bfd_ext_ & 31u);                             \
        if ((w_) == 0u)                                                                                                \
            (w_) = 32u;                                                                                                \
        (dn_) = (uint32_t)((bfd_ext_ >> 12) & 7u);                                                                     \
    } while (0)

// Template for register bit-field ops: extract field, update CC, then apply optional writeback.
// __VA_ARGS__ is the (optional) write-back statement, e.g. DY = bf_insert_reg(DY, off_, w_, val);
#define BF_DN_OP(...)                                                                                                  \
    OP({                                                                                                               \
        int32_t off_;                                                                                                  \
        uint32_t w_;                                                                                                   \
        BF_DECODE_EXT(off_, w_);                                                                                       \
        uint32_t f_ = bf_extract_reg(DY, off_, w_);                                                                    \
        BF_UPDATE_CC(f_, w_);                                                                                          \
        __VA_ARGS__                                                                                                    \
    })

// Template for memory bit-field ops: validate EA, extract field, update CC, then apply optional writeback.
// __VA_ARGS__ is the (optional) write-back statement, e.g. BF_INSERT_MEM(ea_, off_, w_, val);
#define BF_EA_OP(modes, ...)                                                                                           \
    OP({                                                                                                               \
        VALID_EA(modes);                                                                                               \
        int32_t off_;                                                                                                  \
        uint32_t w_;                                                                                                   \
        BF_DECODE_EXT(off_, w_);                                                                                       \
        uint32_t ea_ = CALCULATE_EA(1, EA_MODE, EA_REG, true);                                                         \
        uint32_t f_;                                                                                                   \
        BF_EXTRACT_MEM(ea_, off_, w_, f_);                                                                             \
        BF_UPDATE_CC(f_, w_);                                                                                          \
        __VA_ARGS__                                                                                                    \
    })

// --- BFTST/BFCHG/BFCLR/BFSET/BFEXTU/BFEXTS/BFFFO/BFINS ---
#define OP_BFTST_DN BF_DN_OP()
#define OP_BFTST_EA BF_EA_OP(ea_control, )
#define OP_BFCHG_DN BF_DN_OP(DY = bf_insert_reg(DY, off_, w_, ~f_);)
#define OP_BFCHG_EA BF_EA_OP((ea_control & ea_alterable), BF_INSERT_MEM(ea_, off_, w_, ~f_);)
#define OP_BFCLR_DN BF_DN_OP(DY = bf_insert_reg(DY, off_, w_, 0u);)
#define OP_BFCLR_EA BF_EA_OP((ea_control & ea_alterable), BF_INSERT_MEM(ea_, off_, w_, 0u);)
#define OP_BFSET_DN BF_DN_OP(DY = bf_insert_reg(DY, off_, w_, 0xFFFFFFFFu);)
#define OP_BFSET_EA BF_EA_OP((ea_control & ea_alterable), BF_INSERT_MEM(ea_, off_, w_, 0xFFFFFFFFu);)

#define OP_BFEXTU_DN                                                                                                   \
    OP({                                                                                                               \
        int32_t off_;                                                                                                  \
        uint32_t w_;                                                                                                   \
        uint32_t dn_;                                                                                                  \
        BF_DECODE_EXT_WITH_DN(off_, w_, dn_);                                                                          \
        uint32_t f_ = bf_extract_reg(DY, off_, w_);                                                                    \
        BF_UPDATE_CC(f_, w_);                                                                                          \
        D(dn_) = f_;                                                                                                   \
    })
#define OP_BFEXTU_EA                                                                                                   \
    OP({                                                                                                               \
        VALID_EA(ea_control);                                                                                          \
        int32_t off_;                                                                                                  \
        uint32_t w_;                                                                                                   \
        uint32_t dn_;                                                                                                  \
        BF_DECODE_EXT_WITH_DN(off_, w_, dn_);                                                                          \
        uint32_t ea_ = CALCULATE_EA(1, EA_MODE, EA_REG, true);                                                         \
        uint32_t f_;                                                                                                   \
        BF_EXTRACT_MEM(ea_, off_, w_, f_);                                                                             \
        BF_UPDATE_CC(f_, w_);                                                                                          \
        D(dn_) = f_;                                                                                                   \
    })

#define OP_BFEXTS_DN                                                                                                   \
    OP({                                                                                                               \
        int32_t off_;                                                                                                  \
        uint32_t w_;                                                                                                   \
        uint32_t dn_;                                                                                                  \
        BF_DECODE_EXT_WITH_DN(off_, w_, dn_);                                                                          \
        uint32_t f_ = bf_extract_reg(DY, off_, w_);                                                                    \
        BF_UPDATE_CC(f_, w_);                                                                                          \
        D(dn_) = (w_ < 32u) ? (uint32_t)((int32_t)(f_ << (32u - w_)) >> (32u - w_)) : f_;                              \
    })
#define OP_BFEXTS_EA                                                                                                   \
    OP({                                                                                                               \
        VALID_EA(ea_control);                                                                                          \
        int32_t off_;                                                                                                  \
        uint32_t w_;                                                                                                   \
        uint32_t dn_;                                                                                                  \
        BF_DECODE_EXT_WITH_DN(off_, w_, dn_);                                                                          \
        uint32_t ea_ = CALCULATE_EA(1, EA_MODE, EA_REG, true);                                                         \
        uint32_t f_;                                                                                                   \
        BF_EXTRACT_MEM(ea_, off_, w_, f_);                                                                             \
        BF_UPDATE_CC(f_, w_);                                                                                          \
        D(dn_) = (w_ < 32u) ? (uint32_t)((int32_t)(f_ << (32u - w_)) >> (32u - w_)) : f_;                              \
    })

// BFFFO result: the position of the field's first set bit counted from its
// MSB, or the width if the field is zero.  f_ is right-justified in 32 bits,
// so its MSB sits (32 - w) places below bit 31 and clz overcounts by that.
// The caller adds the signed offset with uint32 wraparound, which is what the
// hardware stores for a negative offset (offset + position, two's complement).
static inline uint32_t bf_first_set(uint32_t f, uint32_t w) {
    return f ? (uint32_t)__builtin_clz(f) - (32u - w) : w;
}

#define OP_BFFFO_DN                                                                                                    \
    OP({                                                                                                               \
        int32_t off_;                                                                                                  \
        uint32_t w_;                                                                                                   \
        uint32_t dn_;                                                                                                  \
        BF_DECODE_EXT_WITH_DN(off_, w_, dn_);                                                                          \
        uint32_t f_ = bf_extract_reg(DY, off_, w_);                                                                    \
        BF_UPDATE_CC(f_, w_);                                                                                          \
        D(dn_) = (uint32_t)off_ + bf_first_set(f_, w_);                                                                \
    })
#define OP_BFFFO_EA                                                                                                    \
    OP({                                                                                                               \
        VALID_EA(ea_control);                                                                                          \
        int32_t off_;                                                                                                  \
        uint32_t w_;                                                                                                   \
        uint32_t dn_;                                                                                                  \
        BF_DECODE_EXT_WITH_DN(off_, w_, dn_);                                                                          \
        uint32_t ea_ = CALCULATE_EA(1, EA_MODE, EA_REG, true);                                                         \
        uint32_t f_;                                                                                                   \
        BF_EXTRACT_MEM(ea_, off_, w_, f_);                                                                             \
        BF_UPDATE_CC(f_, w_);                                                                                          \
        D(dn_) = (uint32_t)off_ + bf_first_set(f_, w_);                                                                \
    })

#define OP_BFINS_DN                                                                                                    \
    OP({                                                                                                               \
        int32_t off_;                                                                                                  \
        uint32_t w_;                                                                                                   \
        uint32_t dn_;                                                                                                  \
        BF_DECODE_EXT_WITH_DN(off_, w_, dn_);                                                                          \
        uint32_t mask_ = (w_ == 32u) ? 0xFFFFFFFFu : ((1u << w_) - 1u);                                                \
        uint32_t f_ = D(dn_) & mask_;                                                                                  \
        BF_UPDATE_CC(f_, w_);                                                                                          \
        DY = bf_insert_reg(DY, off_, w_, f_);                                                                          \
    })
#define OP_BFINS_EA                                                                                                    \
    OP({                                                                                                               \
        VALID_EA((ea_control & ea_alterable));                                                                         \
        int32_t off_;                                                                                                  \
        uint32_t w_;                                                                                                   \
        uint32_t dn_;                                                                                                  \
        BF_DECODE_EXT_WITH_DN(off_, w_, dn_);                                                                          \
        uint32_t mask_ = (w_ == 32u) ? 0xFFFFFFFFu : ((1u << w_) - 1u);                                                \
        uint32_t f_ = D(dn_) & mask_;                                                                                  \
        BF_UPDATE_CC(f_, w_);                                                                                          \
        uint32_t ea_ = CALCULATE_EA(1, EA_MODE, EA_REG, true);                                                         \
        BF_INSERT_MEM(ea_, off_, w_, f_);                                                                              \
    })

// --- CHK.W <ea>,Dn and CHK.L <ea>,Dn ---
// M68000PRM: N=1 if Dn < 0, N=0 if Dn > upper bound, otherwise formally
// undefined — but real silicon updates N from Dn's sign even when no
// exception is taken (cputest030 CHK.W/CHK.L pin this down), so N is set
// unconditionally here. Underflow and overflow both raise the CHK exception
// (vector 6); the handler reads N from the stacked SR to tell them apart.
#define OP_CHK_W_EA_DN                                                                                                 \
    OP(                                                                                                                \
        VALID_EA(ea_data); LOAD_EA_WITH_UPDATE(16, src_); int32_t dn_ = (int32_t)(int16_t)(uint16_t)DX;                \
        int32_t b_ = (int32_t)(int16_t)src_; cpu->negative = (dn_ < 0);                                                \
        if (dn_ < 0) { EXC_CHK(); } else if (dn_ > b_) { EXC_CHK(); })

#define OP_CHK_L_EA_DN                                                                                                 \
    OP(                                                                                                                \
        VALID_EA(ea_data); LOAD_EA_WITH_UPDATE(32, bound_); int32_t dn_ = (int32_t)DX; int32_t b_ = (int32_t)bound_;   \
        cpu->negative = (dn_ < 0); if (dn_ < 0) { EXC_CHK(); } else if (dn_ > b_) { EXC_CHK(); })

// --- CHK2/CMP2: Compare with bounds ---
// Extension word: Dn/An:Rn at bits 15:12, IS bit 11 (0=CMP2, 1=CHK2)
// Compares Rn against lower bound at EA and upper bound at EA+size.
#define CHK2_CMP2(bits)                                                                                                \
    {                                                                                                                  \
        uint16_t ext_ = FETCH16();                                                                                     \
        uint32_t da_ = (ext_ >> 15) & 1u;                                                                              \
        uint32_t rn_ = (ext_ >> 12) & 7u;                                                                              \
        int is_chk2_ = (ext_ >> 11) & 1;                                                                               \
        VALID_EA(ea_control);                                                                                          \
        uint32_t ea_ = GET_EA;                                                                                         \
        int32_t lo_, hi_, val_;                                                                                        \
        if (bits == 8) {                                                                                               \
            lo_ = (int32_t)(int8_t)READ8(ea_);                                                                         \
            hi_ = (int32_t)(int8_t)READ8(ea_ + 1);                                                                     \
            val_ = da_ ? (int32_t)A(rn_) : (int32_t)(int8_t)D(rn_);                                                    \
        } else if (bits == 16) {                                                                                       \
            lo_ = (int32_t)(int16_t)READ16(ea_);                                                                       \
            hi_ = (int32_t)(int16_t)READ16(ea_ + 2);                                                                   \
            val_ = da_ ? (int32_t)A(rn_) : (int32_t)(int16_t)D(rn_);                                                   \
        } else {                                                                                                       \
            lo_ = (int32_t)READ32(ea_);                                                                                \
            hi_ = (int32_t)READ32(ea_ + 4);                                                                            \
            val_ = da_ ? (int32_t)A(rn_) : (int32_t)D(rn_);                                                            \
        }                                                                                                              \
        CC_C = 0;                                                                                                      \
        CC_Z = 0;                                                                                                      \
        if (val_ == lo_ || val_ == hi_) {                                                                              \
            CC_Z = 1;                                                                                                  \
        } else {                                                                                                       \
            if (lo_ <= hi_ && (val_ < lo_ || val_ > hi_))                                                              \
                CC_C = 1;                                                                                              \
            if (lo_ > hi_ && val_ > hi_ && val_ < lo_)                                                                 \
                CC_C = 1;                                                                                              \
        }                                                                                                              \
        CC_N = CC_C;                                                                                                   \
        if (is_chk2_ && CC_C)                                                                                          \
            EXC_CHK();                                                                                                 \
    }

#define OP_CHK2_B_EA_DN OP(CHK2_CMP2(8))
#define OP_CHK2_W_EA_DN OP(CHK2_CMP2(16))
#define OP_CHK2_L_EA_DN OP(CHK2_CMP2(32))

// --- CAS: Compare And Swap (stub -- atomic operations not needed for single-CPU) ---
// CAS(bits): shared body for byte/word/long variants.
// Uses token-pasting (READ##bits, WRITE##bits) and UINT(bits) to parameterise by size.
// Restart-safety: CALCULATE_EA(...,true) post-increments / pre-decrements An
// for (An)+ and -(An) modes before the read/write that may fault.  Snapshot
// An up-front and roll back on bus error so the Format-$B retry restarts
// with pre-instruction values.
#define CAS(bits)                                                                                                      \
    VALID_EA((ea_memory & ea_alterable));                                                                              \
    uint16_t ext_ = FETCH16();                                                                                         \
    uint32_t du_ = (ext_ >> 6) & 7u;                                                                                   \
    uint32_t dc_ = ext_ & 7u;                                                                                          \
    uint32_t cas_an_save_ = (EA_MODE == 3 || EA_MODE == 4) ? cpu->a[EA_REG] : 0;                                       \
    uint32_t addr_ = CALCULATE_EA((bits) / 8, EA_MODE, EA_REG, true);                                                  \
    UINT(bits) mem_ = (UINT(bits))READ##bits(addr_);                                                                   \
    UINT(bits) cmp_ = (UINT(bits))D(dc_);                                                                              \
    UINT(bits) res_;                                                                                                   \
    GENERIC_SUB(mem_, cmp_, res_);                                                                                     \
    if (CC_Z) {                                                                                                        \
        WRITE##bits(addr_, (UINT(bits))D(du_));                                                                        \
    } else {                                                                                                           \
        STORE_DN(bits, dc_, mem_);                                                                                     \
    }                                                                                                                  \
    if (__builtin_expect(g_bus_error_pending, 0) && (EA_MODE == 3 || EA_MODE == 4))                                    \
        cpu->a[EA_REG] = cas_an_save_;

#define OP_CAS_B_DC_DU_EA OP({CAS(8)})
#define OP_CAS_W_DC_DU_EA OP({CAS(16)})
#define OP_CAS_L_DC_DU_EA OP({CAS(32)})

// CAS2: two-operand compare-and-swap (stub -- single-CPU, no actual atomicity needed)
// CAS2(bits): shared body for word/long variants.
#define CAS2(bits)                                                                                                     \
    uint16_t e1_ = FETCH16();                                                                                          \
    uint16_t e2_ = FETCH16();                                                                                          \
    uint32_t rn1_ = (e1_ & 0x8000) ? A((e1_ >> 12) & 7) : D((e1_ >> 12) & 7);                                          \
    uint32_t rn2_ = (e2_ & 0x8000) ? A((e2_ >> 12) & 7) : D((e2_ >> 12) & 7);                                          \
    uint32_t dc1_ = e1_ & 7u;                                                                                          \
    uint32_t du1_ = (e1_ >> 6) & 7u;                                                                                   \
    uint32_t dc2_ = e2_ & 7u;                                                                                          \
    uint32_t du2_ = (e2_ >> 6) & 7u;                                                                                   \
    UINT(bits) m1_ = (UINT(bits))READ##bits(rn1_);                                                                     \
    if (__builtin_expect(g_bus_error_pending, 0))                                                                      \
        break; /* CAS2: first read faulted — bail before touching CC or running the second read */                   \
    UINT(bits) m2_ = (UINT(bits))READ##bits(rn2_);                                                                     \
    if (__builtin_expect(g_bus_error_pending, 0))                                                                      \
        break; /* CAS2: second read faulted */                                                                         \
    UINT(bits) r1_;                                                                                                    \
    GENERIC_SUB(m1_, (UINT(bits))D(dc1_), r1_);                                                                        \
    if (CC_Z) {                                                                                                        \
        UINT(bits) r2_;                                                                                                \
        GENERIC_SUB(m2_, (UINT(bits))D(dc2_), r2_);                                                                    \
        if (CC_Z) {                                                                                                    \
            WRITE##bits(rn1_, (UINT(bits))D(du1_));                                                                    \
            WRITE##bits(rn2_, (UINT(bits))D(du2_));                                                                    \
        } else {                                                                                                       \
            STORE_DN(bits, dc1_, m1_);                                                                                 \
            STORE_DN(bits, dc2_, m2_);                                                                                 \
        }                                                                                                              \
    } else {                                                                                                           \
        STORE_DN(bits, dc1_, m1_);                                                                                     \
        STORE_DN(bits, dc2_, m2_);                                                                                     \
    }

#define OP_CAS2_W_DC_DU_RN OP({CAS2(16)})
#define OP_CAS2_L_DC_DU_RN OP({CAS2(32)})

// --- MOVEM reg->mem: skip register list word before EA extensions on illegal ---
// Instruction stream: opcode(2) + reglist(2) + EA_extension_words.
// On invalid EA, real hardware consumes reglist + EA extensions before exception.
#define OP_MOVEM_W_LIST_EA                                                                                             \
    OP({                                                                                                               \
        if (!((((ea_control) & (ea_alterable)) | (ea_min_an)) & (1u << (EA_MODE + (EA_MODE == 7 ? EA_REG : 0))))) {    \
            cpu->pc += 2; /* skip register list word */                                                                \
            skip_ea_extension_words(cpu, EA_MODE, EA_REG, 0);                                                          \
            EXC_ILLEGAL();                                                                                             \
            continue;                                                                                                  \
        }                                                                                                              \
        MOVEM_FROM_REGISTER(opcode, 16);                                                                               \
    })

#define OP_MOVEM_L_LIST_EA                                                                                             \
    OP({                                                                                                               \
        if (!((((ea_control) & (ea_alterable)) | (ea_min_an)) & (1u << (EA_MODE + (EA_MODE == 7 ? EA_REG : 0))))) {    \
            cpu->pc += 2; /* skip register list word */                                                                \
            skip_ea_extension_words(cpu, EA_MODE, EA_REG, 0);                                                          \
            EXC_ILLEGAL();                                                                                             \
            continue;                                                                                                  \
        }                                                                                                              \
        MOVEM_FROM_REGISTER(opcode, 32);                                                                               \
    })

// --- MOVE SR,<ea>: privileged on 68030 ---
#define OP_MOVE_W_SR_EA OP(VALID_EA(ea_data &ea_alterable); SUPER(STORE_EA(16, GET_SR())))

// TST: 68030 expanded address modes (all EA).
#define OP_TST_B_EA OP(VALID_EA(ea_any - ea_an); LOAD_EA_WITH_UPDATE(8, ea); UPDATE_NZ_CLEAR_CV(ea))
#define OP_TST_W_EA OP(VALID_EA(ea_any); LOAD_EA_WITH_UPDATE(16, ea); UPDATE_NZ_CLEAR_CV(ea))
#define OP_TST_L_EA OP(VALID_EA(ea_any); LOAD_EA_WITH_UPDATE(32, ea); UPDATE_NZ_CLEAR_CV(ea))

// --- MULS.L / DIVS.L: 32x32->64 multiply, 64/32 divide ---
// MULS.L <ea>,Dh:Dl  (or MULU.L)
// Extension word: 0 Dl[14:12] 1 Size[10] 0000000 Dh[2:0]  (per M68000 PRM)
// Size=0: 32-bit product in Dl only; Size=1: 64-bit product in Dh:Dl
// When Size=1 and Dh==Dl: result is undefined per PRM; real 68030 stores
// low then high, so Dl ends up with the high 32 bits.
#define OP_MULS_L_EA_DH_DL                                                                                             \
    OP({                                                                                                               \
        uint16_t ext_ = FETCH16();                                                                                     \
        uint32_t dl_ = (ext_ >> 12) & 7u;                                                                              \
        uint32_t dh_ = ext_ & 7u;                                                                                      \
        int signed_ = (ext_ >> 11) & 1;                                                                                \
        int size64_ = (ext_ >> 10) & 1;                                                                                \
        VALID_EA(ea_data);                                                                                             \
        LOAD_EA_WITH_UPDATE(32, src_);                                                                                 \
        if (signed_) {                                                                                                 \
            int64_t res_ = (int64_t)(int32_t)D(dl_) * (int64_t)(int32_t)src_;                                          \
            D(dl_) = (uint32_t)res_;                                                                                   \
            if (size64_)                                                                                               \
                D(dh_) = (uint32_t)((uint64_t)res_ >> 32);                                                             \
            if (size64_) {                                                                                             \
                CC_N = (res_ < 0);                                                                                     \
                CC_Z = (res_ == 0);                                                                                    \
                CC_V = 0;                                                                                              \
            } else {                                                                                                   \
                CC_N = ((int32_t)D(dl_) < 0);                                                                          \
                CC_Z = (D(dl_) == 0);                                                                                  \
                /* V=1 if high 32 bits are not sign-extension of bit 31 */                                             \
                CC_V = (res_ != (int64_t)(int32_t)D(dl_));                                                             \
            }                                                                                                          \
            CC_C = 0;                                                                                                  \
        } else {                                                                                                       \
            uint64_t res_ = (uint64_t)D(dl_) * (uint64_t)src_;                                                         \
            D(dl_) = (uint32_t)res_;                                                                                   \
            if (size64_)                                                                                               \
                D(dh_) = (uint32_t)(res_ >> 32);                                                                       \
            if (size64_) {                                                                                             \
                CC_N = (res_ >> 63);                                                                                   \
                CC_Z = (res_ == 0);                                                                                    \
                CC_V = 0;                                                                                              \
            } else {                                                                                                   \
                CC_N = D(dl_) >> 31;                                                                                   \
                CC_Z = (D(dl_) == 0);                                                                                  \
                /* V=1 if high 32 bits of 64-bit product are non-zero */                                               \
                CC_V = ((res_ >> 32) != 0);                                                                            \
            }                                                                                                          \
            CC_C = 0;                                                                                                  \
        }                                                                                                              \
    })

// DIVS.L/DIVU.L <ea>,Dr:Dq  (64/32 or 32/32 divide)
// Extension word: 0 Dq[14:12] 1 Size[10] 0000000 Dr[2:0]  (per M68000 PRM)
// Size=0: 32-bit dividend from Dq; Size=1: 64-bit dividend from Dr:Dq
// When Dr==Dq: quotient in Dq, no separate remainder stored
#define OP_DIVS_L_EA_DR_DQ                                                                                             \
    OP({                                                                                                               \
        uint16_t ext_ = FETCH16();                                                                                     \
        uint32_t dq_ = (ext_ >> 12) & 7u;                                                                              \
        uint32_t dr_ = ext_ & 7u;                                                                                      \
        int signed_ = (ext_ >> 11) & 1;                                                                                \
        int size64_ = (ext_ >> 10) & 1;                                                                                \
        VALID_EA(ea_data);                                                                                             \
        LOAD_EA_WITH_UPDATE(32, divisor_);                                                                             \
        CLEAR_NZVC();                                                                                                  \
        if (!divisor_) {                                                                                               \
            EXC_DIVIDE_BY_ZERO();                                                                                      \
        } else {                                                                                                       \
            if (signed_) {                                                                                             \
                int64_t dividend_ = size64_ ? (int64_t)(((uint64_t)D(dr_) << 32) | D(dq_)) : (int64_t)(int32_t)D(dq_); \
                /* INT64_MIN / -1 has no representable quotient.  The C division is UB,                                \
                 * and the shipping wasm build's i64.div_s traps on it by specification,                               \
                 * so the test must precede the divide -- inspecting q_ cannot work. */                                \
                if (dividend_ == INT64_MIN && (int32_t)divisor_ == -1) {                                               \
                    CC_V = CC_N = 1;                                                                                   \
                } else {                                                                                               \
                    int64_t q_ = dividend_ / (int32_t)divisor_;                                                        \
                    int64_t r_ = dividend_ % (int32_t)divisor_;                                                        \
                    if (q_ > INT32_MAX || q_ < INT32_MIN) {                                                            \
                        CC_V = CC_N = 1;                                                                               \
                    } else {                                                                                           \
                        D(dq_) = (uint32_t)q_;                                                                         \
                        if (dr_ != dq_)                                                                                \
                            D(dr_) = (uint32_t)r_;                                                                     \
                        CC_N = (q_ < 0);                                                                               \
                        CC_Z = (q_ == 0);                                                                              \
                    }                                                                                                  \
                }                                                                                                      \
            } else {                                                                                                   \
                uint64_t dividend_ = size64_ ? (((uint64_t)D(dr_) << 32) | D(dq_)) : (uint64_t)D(dq_);                 \
                uint64_t q_ = dividend_ / (uint32_t)divisor_;                                                          \
                uint64_t r_ = dividend_ % (uint32_t)divisor_;                                                          \
                if (q_ > UINT32_MAX) {                                                                                 \
                    CC_V = CC_N = 1;                                                                                   \
                } else {                                                                                               \
                    D(dq_) = (uint32_t)q_;                                                                             \
                    if (dr_ != dq_)                                                                                    \
                        D(dr_) = (uint32_t)r_;                                                                         \
                    CC_N = (uint32_t)q_ >> 31;                                                                         \
                    CC_Z = (q_ == 0);                                                                                  \
                }                                                                                                      \
            }                                                                                                          \
        }                                                                                                              \
    })

// --- RTD: Return and Deallocate ---
#define OP_RTD_DISPLACEMENT                                                                                            \
    OP({                                                                                                               \
        int16_t d_ = (int16_t)FETCH16(); /* read displacement before popping return address */                         \
        POP32(PC);                                                                                                     \
        SP += (int32_t)d_;                                                                                             \
    })

// --- MOVEC: Move Control Register ---
// Implementations live in cpu_68030.c (cpu_movec_rc_rn / cpu_movec_rn_rc).
// The functions fetch the extension word, perform the operation and return 0 on illegal Rc.
#define OP_MOVEC_RC_RN OP(SUPER(if (!cpu_movec_rc_rn(cpu)) continue;))
#define OP_MOVEC_RN_RC OP(SUPER(if (!cpu_movec_rn_rc(cpu)) continue;))

// --- RTE: Return from Exception (68030 format word parsing) ---
#define OP_RTE                                                                                                         \
    OP(SUPER({                                                                                                         \
        /* Read SR, PC, format from stack without advancing SP */                                                      \
        uint16_t sr_ = memory_read_uint16(SP);                                                                         \
        uint32_t pc_ = memory_read_uint32(SP + 2);                                                                     \
        uint16_t fmt_ = memory_read_uint16(SP + 6);                                                                    \
        int format_ = (fmt_ >> 12) & 0xF;                                                                              \
        int offset_ = 8; /* base frame size: SR(2) + PC(4) + fmt/vec(2) */                                             \
        int fmterr_ = 0;                                                                                               \
        switch (format_) {                                                                                             \
        case 0x0:                                                                                                      \
            break; /* 4-word frame, no extra data */                                                                   \
        case 0x2:                                                                                                      \
            offset_ += 4;                                                                                              \
            break; /* 6-word frame: +instruction address */                                                            \
        case 0x3:                                                                                                      \
            offset_ += 4;                                                                                              \
            break; /* 68040 FP post-instruction frame: +effective address */                                           \
        case 0x7:                                                                                                      \
            /* MC68040 access error (30-word) frame.  Writebacks are never  */                                         \
            /* pending in this functional model, so the WBxS fields the     */                                         \
            /* handler may have completed are simply discarded.             */                                         \
            if (cpu->cpu_model >= CPU_MODEL_68040)                                                                     \
                offset_ += 52;                                                                                         \
            else                                                                                                       \
                fmterr_ = 1;                                                                                           \
            break;                                                                                                     \
        case 0x9:                                                                                                      \
            offset_ += 12;                                                                                             \
            break; /* coprocessor mid-instruction (+12) */                                                             \
        case 0xA:                                                                                                      \
            offset_ += 24;                                                                                             \
            break; /* short bus fault (+24) */                                                                         \
        case 0xB:                                                                                                      \
            offset_ += 84;                                                                                             \
            break; /* long bus fault (+84) */                                                                          \
        default:                                                                                                       \
            fmterr_ = 1;                                                                                               \
            break;                                                                                                     \
        }                                                                                                              \
        if (fmterr_) {                                                                                                 \
            /* Invalid format: clear trace bits, format error (vector 14) */                                           \
            /* SP is NOT advanced; stacked SR is the current (pre-RTE) SR */                                           \
            cpu->trace = 0;                                                                                            \
            exception(cpu, 0x038, cpu->instruction_pc, GET_SR());                                                      \
        } else {                                                                                                       \
            /* Valid format: advance SP past entire frame, apply new SR and PC */                                      \
            SP += offset_;                                                                                             \
            PC = pc_;                                                                                                  \
            SET_SR(sr_);                                                                                               \
        }                                                                                                              \
    }))

// --- EXTB.L: override LEA to handle EXTB.L Dn (mode=0 EA) ---
// EXTB.L Dn is encoded as opcode 0x49C0+Dn, which falls in the LEA A4 case.
// When EA mode == 0 and register field == 4 (A4 slot), treat as EXTB.L.
// For other An slots with EA mode == 0 (Dn), fall through to VALID_EA -> ILLEGAL.
#define OP_LEA_EA_AN                                                                                                   \
    OP(                                                                                                                \
        if (EA_MODE == 0 && ((opcode >> 9) & 7) == 4) {                                                                \
            uint32_t r_ = (uint32_t)(int32_t)(int8_t)DY;                                                               \
            DY = r_;                                                                                                   \
            UPDATE_NZ_CLEAR_CV(r_);                                                                                    \
        } else {                                                                                                       \
            VALID_EA(ea_control);                                                                                      \
            AX = GET_EA;                                                                                               \
        })

// --- TRAPcc: Trap on Condition ---
#define OP_TRAPCC OP(if (CC) EXC_TRAPV())

#define OP_TRAPCC_W_DATA                                                                                               \
    OP({                                                                                                               \
        (void)FETCH16();                                                                                               \
        if (CC)                                                                                                        \
            EXC_TRAPV();                                                                                               \
    })

#define OP_TRAPCC_L_DATA                                                                                               \
    OP({                                                                                                               \
        (void)FETCH32();                                                                                               \
        if (CC)                                                                                                        \
            EXC_TRAPV();                                                                                               \
    })

// --- PACK/UNPK: BCD pack/unpack ---
// PACK DY,DX,#adj: ((DY + adj) & 0xFF) -> low byte nibbles -> DX
#define OP_PACK_DY_DX                                                                                                  \
    OP({                                                                                                               \
        uint16_t adj_ = FETCH16();                                                                                     \
        uint16_t src_ = (uint16_t)(DY + adj_);                                                                         \
        STORE_DN(8, opcode >> 9 & 7, ((src_ >> 4) & 0xF0) | ((src_) & 0x0F));                                          \
    })

// PACK -(AY),-(AX),#adj: from memory.  Each predec mutates An before the
// access that may fault; snapshot both An and roll back on bus error so the
// Format-$B retry restarts with pre-instruction values.
#define OP_PACK_AY_AX                                                                                                  \
    OP({                                                                                                               \
        uint16_t adj_ = FETCH16();                                                                                     \
        int dx_ = opcode >> 9 & 7;                                                                                     \
        uint32_t pack_ay_save_ = cpu->a[EA_REG];                                                                       \
        uint32_t pack_ax_save_ = cpu->a[dx_];                                                                          \
        A(EA_REG) -= 2;                                                                                                \
        uint16_t src_ = READ16(A(EA_REG));                                                                             \
        src_ = (uint16_t)(src_ + adj_);                                                                                \
        A(dx_) -= (dx_ == 7) ? 2 : 1; /* A7 byte predec keeps stack word-aligned */                                    \
        WRITE8(A(dx_), (uint8_t)(((src_ >> 4) & 0xF0) | ((src_) & 0x0F)));                                             \
        if (__builtin_expect(g_bus_error_pending, 0)) {                                                                \
            cpu->a[EA_REG] = pack_ay_save_;                                                                            \
            cpu->a[dx_] = pack_ax_save_;                                                                               \
        }                                                                                                              \
    })

// UNPK DY,DX,#adj: separate two BCD nibbles, add adj
#define OP_UNPK_DY_DX                                                                                                  \
    OP({                                                                                                               \
        uint16_t adj_ = FETCH16();                                                                                     \
        uint8_t b_ = (uint8_t)DY;                                                                                      \
        uint16_t res_ = (uint16_t)((((b_ >> 4) & 0xF) << 8) | ((b_) & 0xF)) + adj_;                                    \
        STORE_DN(16, opcode >> 9 & 7, res_);                                                                           \
    })

// UNPK -(AY),-(AX),#adj: from/to memory.  Same restart-safety concern as
// OP_PACK_AY_AX — snapshot both An and roll back on bus error.
#define OP_UNPK_AY_AX                                                                                                  \
    OP({                                                                                                               \
        uint16_t adj_ = FETCH16();                                                                                     \
        int sy_ = EA_REG;                                                                                              \
        int dx_ = opcode >> 9 & 7;                                                                                     \
        uint32_t unpk_ay_save_ = cpu->a[sy_];                                                                          \
        uint32_t unpk_ax_save_ = cpu->a[dx_];                                                                          \
        A(sy_) -= (sy_ == 7) ? 2 : 1; /* A7 byte predec keeps stack word-aligned */                                    \
        uint8_t b_ = READ8(A(sy_));                                                                                    \
        uint16_t res_ = (uint16_t)((((b_ >> 4) & 0xF) << 8) | ((b_) & 0xF)) + adj_;                                    \
        A(dx_) -= 2;                                                                                                   \
        WRITE16(A(dx_), res_);                                                                                         \
        if (__builtin_expect(g_bus_error_pending, 0)) {                                                                \
            cpu->a[sy_] = unpk_ay_save_;                                                                               \
            cpu->a[dx_] = unpk_ax_save_;                                                                               \
        }                                                                                                              \
    })

// --- MOVES: Move with Alternate Function Code ---
// Reads/writes a single operand using the FC in SFC (for EA→Rn reads) or DFC
// (for Rn→EA writes) instead of the CPU's current FC.  A/UX kernels set
// SFC=DFC=1 (user data) and then use MOVES from supervisor mode to touch
// user pages (copyin/copyout/copyinstr).  We honor this by temporarily
// pointing g_active_read / g_active_write at the user SoA tables when the
// FC bit 2 is clear (1/2 = user data/program), and at the supervisor tables
// when bit 2 is set (5/6 = supervisor data/program).  The hot-loop epilogue
// re-establishes g_active_read/write based on cpu->supervisor, so any bus
// error raised by the swapped access still finds the correct tables when
// the handler resumes.
//
// For autoincrement/autodecrement EA modes, CALCULATE_EA is invoked with
// increment=false so the address register is NOT updated before the memory
// access — otherwise a page-fault retry would re-run CALCULATE_EA and
// increment/decrement the register a second time, writing 4 bytes off from
// the intended address.  After the memory access succeeds we commit the
// register update manually.  A deferred bus error aborts before the commit,
// leaving the register at its pre-instruction value so the retry places the
// write at the correct address.
#define MOVES_ALT_READ(FC)  ((((FC)) & 4u) ? g_supervisor_read : g_user_read)
#define MOVES_ALT_WRITE(FC) ((((FC)) & 4u) ? g_supervisor_write : g_user_write)

#define MOVES_EA_WITH_RETRY_SAFE(SIZE, VAR)                                                                            \
    uint32_t VAR;                                                                                                      \
    bool moves_autoinc_ = false, moves_autodec_ = false;                                                               \
    if (EA_MODE == 3) { /* (An)+ */                                                                                    \
        VAR = A(EA_REG);                                                                                               \
        moves_autoinc_ = true;                                                                                         \
    } else if (EA_MODE == 4) { /* -(An) */                                                                             \
        VAR = A(EA_REG) - ((EA_REG == 7 && (SIZE) == 1) ? 2 : (SIZE));                                                 \
        moves_autodec_ = true;                                                                                         \
    } else {                                                                                                           \
        VAR = CALCULATE_EA((SIZE), EA_MODE, EA_REG, true);                                                             \
    }

#define MOVES_COMMIT_EA(SIZE)                                                                                          \
    if (moves_autoinc_)                                                                                                \
        A(EA_REG) += (EA_REG == 7 && (SIZE) == 1) ? 2 : (SIZE);                                                        \
    else if (moves_autodec_)                                                                                           \
        A(EA_REG) -= (EA_REG == 7 && (SIZE) == 1) ? 2 : (SIZE);

// Source value for MOVES Rn,<ea>.  When Rn is the same address register used
// by an (An)+/-(An) EA, real 68020/030 silicon stores the UPDATED register
// value even though we only commit the register after the write succeeds
// (verified against cputest030 MOVES.B/W/L).  For -(An) the updated value is
// the effective address itself.
#define MOVES_RN_SRC(SIZE, DA, RN, EA)                                                                                 \
    ((DA) ? (((RN) == (uint32_t)EA_REG && moves_autoinc_)                                                              \
                 ? A(RN) + ((EA_REG == 7 && (SIZE) == 1) ? 2u : (uint32_t)(SIZE))                                      \
                 : (((RN) == (uint32_t)EA_REG && moves_autodec_) ? (EA) : A(RN)))                                      \
          : D(RN))

#define OP_MOVES_B_RN_EA                                                                                               \
    OP({                                                                                                               \
        VALID_EA(ea_memory &ea_alterable);                                                                             \
        SUPER({                                                                                                        \
            uint16_t ext_ = FETCH16();                                                                                 \
            uint32_t da_ = (ext_ >> 15) & 1u;                                                                          \
            uint32_t rn_ = (ext_ >> 12) & 7u;                                                                          \
            MOVES_EA_WITH_RETRY_SAFE(1, ea_);                                                                          \
            uintptr_t *saved_ = g_active_write;                                                                        \
            g_active_write = MOVES_ALT_WRITE(cpu->dfc);                                                                \
            WRITE8(ea_, (uint8_t)MOVES_RN_SRC(1, da_, rn_, ea_));                                                      \
            g_active_write = saved_;                                                                                   \
            if (!g_bus_error_pending) {                                                                                \
                MOVES_COMMIT_EA(1);                                                                                    \
            }                                                                                                          \
        });                                                                                                            \
    })

#define OP_MOVES_W_RN_EA                                                                                               \
    OP({                                                                                                               \
        VALID_EA(ea_memory &ea_alterable);                                                                             \
        SUPER({                                                                                                        \
            uint16_t ext_ = FETCH16();                                                                                 \
            uint32_t da_ = (ext_ >> 15) & 1u;                                                                          \
            uint32_t rn_ = (ext_ >> 12) & 7u;                                                                          \
            MOVES_EA_WITH_RETRY_SAFE(2, ea_);                                                                          \
            uintptr_t *saved_ = g_active_write;                                                                        \
            g_active_write = MOVES_ALT_WRITE(cpu->dfc);                                                                \
            WRITE16(ea_, (uint16_t)MOVES_RN_SRC(2, da_, rn_, ea_));                                                    \
            g_active_write = saved_;                                                                                   \
            if (!g_bus_error_pending) {                                                                                \
                MOVES_COMMIT_EA(2);                                                                                    \
            }                                                                                                          \
        });                                                                                                            \
    })

#define OP_MOVES_L_RN_EA                                                                                               \
    OP({                                                                                                               \
        VALID_EA(ea_memory &ea_alterable);                                                                             \
        SUPER({                                                                                                        \
            uint16_t ext_ = FETCH16();                                                                                 \
            uint32_t da_ = (ext_ >> 15) & 1u;                                                                          \
            uint32_t rn_ = (ext_ >> 12) & 7u;                                                                          \
            MOVES_EA_WITH_RETRY_SAFE(4, ea_);                                                                          \
            uintptr_t *saved_ = g_active_write;                                                                        \
            g_active_write = MOVES_ALT_WRITE(cpu->dfc);                                                                \
            WRITE32(ea_, MOVES_RN_SRC(4, da_, rn_, ea_));                                                              \
            g_active_write = saved_;                                                                                   \
            if (!g_bus_error_pending) {                                                                                \
                MOVES_COMMIT_EA(4);                                                                                    \
            }                                                                                                          \
        });                                                                                                            \
    })

#define OP_MOVES_B_EA_RN                                                                                               \
    OP({                                                                                                               \
        VALID_EA(ea_memory &ea_alterable);                                                                             \
        SUPER({                                                                                                        \
            uint16_t ext_ = FETCH16();                                                                                 \
            uint32_t da_ = (ext_ >> 15) & 1u;                                                                          \
            uint32_t rn_ = (ext_ >> 12) & 7u;                                                                          \
            MOVES_EA_WITH_RETRY_SAFE(1, ea_);                                                                          \
            uintptr_t *saved_ = g_active_read;                                                                         \
            g_active_read = MOVES_ALT_READ(cpu->sfc);                                                                  \
            uint8_t v_ = READ8(ea_);                                                                                   \
            g_active_read = saved_;                                                                                    \
            if (!g_bus_error_pending) {                                                                                \
                MOVES_COMMIT_EA(1);                                                                                    \
                if (da_)                                                                                               \
                    A(rn_) = (int32_t)(int8_t)v_;                                                                      \
                else                                                                                                   \
                    STORE_DN(8, rn_, v_);                                                                              \
            }                                                                                                          \
        });                                                                                                            \
    })

#define OP_MOVES_W_EA_RN                                                                                               \
    OP({                                                                                                               \
        VALID_EA(ea_memory &ea_alterable);                                                                             \
        SUPER({                                                                                                        \
            uint16_t ext_ = FETCH16();                                                                                 \
            uint32_t da_ = (ext_ >> 15) & 1u;                                                                          \
            uint32_t rn_ = (ext_ >> 12) & 7u;                                                                          \
            MOVES_EA_WITH_RETRY_SAFE(2, ea_);                                                                          \
            uintptr_t *saved_ = g_active_read;                                                                         \
            g_active_read = MOVES_ALT_READ(cpu->sfc);                                                                  \
            uint16_t v_ = READ16(ea_);                                                                                 \
            g_active_read = saved_;                                                                                    \
            if (!g_bus_error_pending) {                                                                                \
                MOVES_COMMIT_EA(2);                                                                                    \
                if (da_)                                                                                               \
                    A(rn_) = (int32_t)(int16_t)v_;                                                                     \
                else                                                                                                   \
                    STORE_DN(16, rn_, v_);                                                                             \
            }                                                                                                          \
        });                                                                                                            \
    })

#define OP_MOVES_L_EA_RN                                                                                               \
    OP({                                                                                                               \
        VALID_EA(ea_memory &ea_alterable);                                                                             \
        SUPER({                                                                                                        \
            uint16_t ext_ = FETCH16();                                                                                 \
            uint32_t da_ = (ext_ >> 15) & 1u;                                                                          \
            uint32_t rn_ = (ext_ >> 12) & 7u;                                                                          \
            MOVES_EA_WITH_RETRY_SAFE(4, ea_);                                                                          \
            uintptr_t *saved_ = g_active_read;                                                                         \
            g_active_read = MOVES_ALT_READ(cpu->sfc);                                                                  \
            uint32_t v_ = READ32(ea_);                                                                                 \
            g_active_read = saved_;                                                                                    \
            if (!g_bus_error_pending) {                                                                                \
                MOVES_COMMIT_EA(4);                                                                                    \
                if (da_)                                                                                               \
                    A(rn_) = v_;                                                                                       \
                else                                                                                                   \
                    D(rn_) = v_;                                                                                       \
            }                                                                                                          \
        });                                                                                                            \
    })

// --- BKPT: Software Breakpoint (generate BKPT trap = vector 4 illegal instruction) ---
// Push the opcode's own address, not the advanced pc.
#define OP_BKPT_DATA OP(exception(cpu, 0x010, cpu->instruction_pc, cpu_get_sr(cpu)))

// --- MOVE CCR,<ea>: read CCR into EA as a word (not privileged on 68010+) ---
#define OP_MOVE_B_CCR_EA OP(VALID_EA(ea_data &ea_alterable); STORE_EA(16, READ_CCR()))

// --- LINK.L (32-bit displacement) ---
// Fetch the displacement BEFORE touching SP or An, mirroring OP_LINK.  With
// the fetch last, a page fault on the immediate left An and SP already
// updated, so the Format $B retry re-ran the push and double-linked the
// frame.  68020+ only (the 68000 arm maps this to OP_UNDEFINED), so reaching
// it needs a PMMU or an 040.
#define OP_LINK_L_AN_DISP                                                                                              \
    OP({                                                                                                               \
        int32_t disp_ = (int32_t)FETCH32();                                                                            \
        uint32_t a_ = AY;                                                                                              \
        PUSH(a_);                                                                                                      \
        AY = SP;                                                                                                       \
        SP += disp_;                                                                                                   \
    })

// --- MMU branch conditionals: stub as not-taken (MMU conditions always false) ---
#define OP_PBCC_W OP(PC += 2) // skip branch displacement, never branch
#define OP_PBCC_L OP(PC += 4) // skip long branch displacement, never branch

// --- MMU PSAVE / PRESTORE: save/restore PMMU internal state ---
//
// On real MC68030 hardware PSAVE/PRESTORE are not supported (they F-trap);
// they are MC68851 PMMU instructions that survived into the shared CpID=0
// opcode space. Most 68030 OSes (including A/UX 3.0.1) save/restore the
// externally-visible MMU registers (TC, SRP, CRP, TT0, TT1) via PMOVE and
// never execute PSAVE/PRESTORE on the 030. Implementing them as no-ops
// used to work but creates a silent correctness hazard the moment any
// code path does reach them.
//
// Since the emulator's MMU is always in a synchronous idle state between
// instructions (no mid-table-walk abort to preserve), PSAVE writes a 4-byte
// null-idle frame (V=0, LEN=0) — the MC68851 "null frame" format — and
// PRESTORE reads it back and drops it. Both address EAs are fully updated
// for predecrement/postincrement per the real instructions' operand modes.
//
// EA modes accepted:
//   PSAVE:    control alterable + predecrement   ((ea_control|ea_min_an) & ea_alterable)
//   PRESTORE: control            + postincrement   (ea_control|ea_an_plus)
#define OP_PSAVE_EA                                                                                                    \
    OP(SUPER({                                                                                                         \
        VALID_EA((ea_control | ea_min_an) & ea_alterable);                                                             \
        uint32_t ea_ = GET_EA;                                                                                         \
        WRITE32(ea_, 0);                                                                                               \
    }))

#define OP_PRESTORE_EA                                                                                                 \
    OP(SUPER({                                                                                                         \
        VALID_EA(ea_control | ea_an_plus);                                                                             \
        uint32_t ea_ = GET_EA;                                                                                         \
        (void)READ32(ea_);                                                                                             \
    }))

// --- CpID=0 type=0: 68030 MMU general instruction (PMOVE/PFLUSH/PTEST/PLOAD) ---
// Decodes the extension word to determine the specific MMU operation.
// All are privileged (supervisor-only).
#define OP_PMMU_GENERAL OP(SUPER(cpu_pmmu_general(cpu, opcode)))

// --- FPU FSAVE/FRESTORE/FBcc: 68030 with no FPU -> Line-F exception ---
// Without a physical 68881/68882, the coprocessor interface has no responder.
// All CpID=1 FPU instructions generate F-line exceptions, just like 68000.
// f_trap() stacks PC pointing to the F-line instruction word (cpu->pc - 2).
#define OP_FSAVE_EA            OP(EXC_FTRAP())
#define OP_FRESTORE_EA         OP(EXC_FTRAP())
#define OP_FBCC_W_DISPLACEMENT OP(EXC_FTRAP())
#define OP_FBCC_L_DISPLACEMENT OP(EXC_FTRAP())

// --- FTRAP default: any F-line opcode the dispatcher didn't recognise ---
//
// Real hardware dispatches the opcode to the coprocessor matching CpID. If
// the coprocessor reports "unimplemented" (or there is no coprocessor), the
// CPU raises a line-F trap (vector 11). Privilege violation (vector 8) is
// only the right answer when the coprocessor itself recognised the opcode
// and reports privilege error — which we don't model here, since this is
// the catch-all for *unrecognised* opcodes only.
#define OP_FTRAP                                                                                                       \
    OP({                                                                                                               \
        if (((opcode >> 9) & 7u) == 0u) {                                                                              \
            /* CpID=0: 68030 PMMU coprocessor space. Sub-types we implement   */                                       \
            /* are routed elsewhere by cpu_decode.h. Reaching here means the  */                                       \
            /* sub-type is unimplemented — emit F-line trap (vector 11).      */                                     \
            EXC_FTRAP();                                                                                               \
        } else {                                                                                                       \
            /* cpSAVE (type=4) / cpRESTORE (type=5) are privileged when EA is valid */                                 \
            uint32_t cotype_ = (opcode >> 6) & 7u;                                                                     \
            uint32_t ea_bit_ = 1u << (EA_MODE + (EA_MODE == 7 ? EA_REG : 0));                                          \
            /* cpSAVE: control alterable + predecrement; cpRESTORE: control + postincrement */                         \
            uint32_t save_ea_ = ((ea_control) & (ea_alterable)) | (ea_min_an);                                         \
            uint32_t rest_ea_ = (ea_control) | (ea_an_plus);                                                           \
            if ((cotype_ == 4u && (save_ea_ & ea_bit_)) || (cotype_ == 5u && (rest_ea_ & ea_bit_))) {                  \
                SUPER(EXC_FTRAP());                                                                                    \
            } else {                                                                                                   \
                EXC_FTRAP();                                                                                           \
            }                                                                                                          \
        }                                                                                                              \
    })

// General FPU operation (type=0): arithmetic, FMOVE, FMOVEM, FMOVECR
#define OP_FPU_GENERAL OP(EXC_FTRAP())

// FScc/FDBcc/FTRAPcc (type=1)
#define OP_FPU_SCCDBCC OP(EXC_FTRAP())

// ============================================================================
// 68882 FPU instruction macros (CpID=1)
// ============================================================================

// General FPU operation (type=0): arithmetic, FMOVE, FMOVEM, FMOVECR
#undef OP_FPU_GENERAL
#define OP_FPU_GENERAL                                                                                                 \
    OP({                                                                                                               \
        if (!cpu->fpu) {                                                                                               \
            EXC_FTRAP();                                                                                               \
        } else {                                                                                                       \
            uint16_t ext_ = FETCH16();                                                                                 \
            fpu_general_op(cpu, (fpu_state_t *)cpu->fpu, opcode, ext_);                                                \
        }                                                                                                              \
    })

// FScc/FDBcc/FTRAPcc (type=1)
#undef OP_FPU_SCCDBCC
#define OP_FPU_SCCDBCC                                                                                                 \
    OP({                                                                                                               \
        if (!cpu->fpu) {                                                                                               \
            EXC_FTRAP();                                                                                               \
        } else {                                                                                                       \
            fpu_state_t *fpu_ = (fpu_state_t *)cpu->fpu;                                                               \
            fpu_->initialized = true;                                                                                  \
            unsigned mode_ = (opcode >> 3) & 7;                                                                        \
            unsigned reg_ = opcode & 7;                                                                                \
            uint16_t ext_ = FETCH16();                                                                                 \
            unsigned cond_ = ext_ & 0x3F;                                                                              \
            fpu_->fpiar = cpu->instruction_pc;                                                                         \
            bool cc_ = fpu_test_condition(fpu_, cond_);                                                                \
            if (mode_ == 1) {                                                                                          \
                /* FDBcc: decrement Dn, branch if !cc && Dn != -1 */                                                   \
                int16_t disp_ = (int16_t)FETCH16();                                                                    \
                if (!cc_) {                                                                                            \
                    int16_t cnt_ = (int16_t)(uint16_t)D(reg_) - 1;                                                     \
                    D(reg_) = (D(reg_) & 0xFFFF0000u) | (uint16_t)cnt_;                                                \
                    if (cnt_ != -1)                                                                                    \
                        PC = cpu->instruction_pc + 4 + (int32_t)disp_;                                                 \
                }                                                                                                      \
            } else if (mode_ == 7 && reg_ >= 2) {                                                                      \
                /* FTRAPcc: optional immediate operand + trap if cc */                                                 \
                if (reg_ == 2)                                                                                         \
                    (void)FETCH16();                                                                                   \
                else if (reg_ == 3)                                                                                    \
                    (void)FETCH32();                                                                                   \
                if (cc_)                                                                                               \
                    EXC_TRAPV();                                                                                       \
            } else {                                                                                                   \
                /* FScc: set byte at EA to $FF if cc, $00 otherwise */                                                 \
                WRITE_EA(8, mode_, reg_, cc_ ? 0xFF : 0x00);                                                           \
            }                                                                                                          \
            /* Check for BSUN exception after instruction completes */                                                 \
            fpu_check_exceptions(cpu, fpu_);                                                                           \
        }                                                                                                              \
    })

// FBcc.W: branch on FPU condition with 16-bit displacement
#undef OP_FBCC_W_DISPLACEMENT
#define OP_FBCC_W_DISPLACEMENT                                                                                         \
    OP({                                                                                                               \
        if (!cpu->fpu) {                                                                                               \
            EXC_FTRAP();                                                                                               \
        } else {                                                                                                       \
            fpu_state_t *fpu_ = (fpu_state_t *)cpu->fpu;                                                               \
            fpu_->initialized = true;                                                                                  \
            int16_t disp_ = (int16_t)FETCH16();                                                                        \
            unsigned cond_ = opcode & 0x3F;                                                                            \
            fpu_->fpiar = cpu->instruction_pc;                                                                         \
            /* Pre-instruction exception check (MC68882UM §6.1.4) */                                                  \
            if (fpu_pre_instruction_check(cpu, fpu_, true))                                                            \
                break;                                                                                                 \
            bool cc_ = fpu_test_condition(fpu_, cond_);                                                                \
            /* If BSUN enabled and fired, take exception instead of branch */                                          \
            if ((fpu_->fpsr & FPEXC_BSUN) && (fpu_->fpcr & FPEXC_BSUN)) {                                              \
                fpu_->fpsr |= FPACC_IOP;                                                                               \
                fpu_check_exceptions(cpu, fpu_);                                                                       \
            } else if (cc_) {                                                                                          \
                PC = cpu->instruction_pc + 2 + (int32_t)disp_;                                                         \
            }                                                                                                          \
        }                                                                                                              \
    })

// FBcc.L: branch on FPU condition with 32-bit displacement
#undef OP_FBCC_L_DISPLACEMENT
#define OP_FBCC_L_DISPLACEMENT                                                                                         \
    OP({                                                                                                               \
        if (!cpu->fpu) {                                                                                               \
            EXC_FTRAP();                                                                                               \
        } else {                                                                                                       \
            fpu_state_t *fpu_ = (fpu_state_t *)cpu->fpu;                                                               \
            fpu_->initialized = true;                                                                                  \
            int32_t disp_ = (int32_t)FETCH32();                                                                        \
            unsigned cond_ = opcode & 0x3F;                                                                            \
            fpu_->fpiar = cpu->instruction_pc;                                                                         \
            bool cc_ = fpu_test_condition(fpu_, cond_);                                                                \
            /* If BSUN enabled and fired, take exception instead of branch */                                          \
            if ((fpu_->fpsr & FPEXC_BSUN) && (fpu_->fpcr & FPEXC_BSUN)) {                                              \
                fpu_->fpsr |= FPACC_IOP;                                                                               \
                fpu_check_exceptions(cpu, fpu_);                                                                       \
            } else if (cc_) {                                                                                          \
                PC = cpu->instruction_pc + 2 + disp_;                                                                  \
            }                                                                                                          \
        }                                                                                                              \
    })

// FSAVE: write idle or null state frame (supervisor only)
#undef OP_FSAVE_EA
#define OP_FSAVE_EA                                                                                                    \
    OP({                                                                                                               \
        if (!cpu->fpu) {                                                                                               \
            EXC_FTRAP();                                                                                               \
        } else                                                                                                         \
            SUPER({                                                                                                    \
                fpu_state_t *fpu_ = (fpu_state_t *)cpu->fpu;                                                           \
                if (EA_MODE == 4) {                                                                                    \
                    /* -(An) predecrement: compute frame size, then write */                                           \
                    int sz_ = fpu_->initialized ? (4 + FSAVE_IDLE_SIZE) : 4;                                           \
                    AY -= (uint32_t)sz_;                                                                               \
                    fpu_fsave(fpu_, AY);                                                                               \
                } else {                                                                                               \
                    uint32_t ea_ = GET_EA;                                                                             \
                    fpu_fsave(fpu_, ea_);                                                                              \
                }                                                                                                      \
            })                                                                                                         \
    })

// FRESTORE: restore FPU state from frame (supervisor only)
#undef OP_FRESTORE_EA
#define OP_FRESTORE_EA                                                                                                 \
    OP({                                                                                                               \
        if (!cpu->fpu) {                                                                                               \
            EXC_FTRAP();                                                                                               \
        } else                                                                                                         \
            SUPER({                                                                                                    \
                fpu_state_t *fpu_ = (fpu_state_t *)cpu->fpu;                                                           \
                if (EA_MODE == 3) {                                                                                    \
                    /* (An)+ postincrement */                                                                          \
                    int sz_ = fpu_frestore(fpu_, AY);                                                                  \
                    AY += (uint32_t)sz_;                                                                               \
                } else {                                                                                               \
                    uint32_t ea_ = GET_EA;                                                                             \
                    fpu_frestore(fpu_, ea_);                                                                           \
                }                                                                                                      \
            })                                                                                                         \
    })

#else // 68000

// OP_UNDEFINED: exception handling
#define OP_UNDEFINED OP(EXC_ILLEGAL(); continue)

// Bit-field instructions: undefined on 68000
#define OP_BFTST_DN  OP_UNDEFINED
#define OP_BFTST_EA  OP_UNDEFINED
#define OP_BFCHG_DN  OP_UNDEFINED
#define OP_BFCHG_EA  OP_UNDEFINED
#define OP_BFCLR_DN  OP_UNDEFINED
#define OP_BFCLR_EA  OP_UNDEFINED
#define OP_BFEXTS_DN OP_UNDEFINED
#define OP_BFEXTS_EA OP_UNDEFINED
#define OP_BFEXTU_DN OP_UNDEFINED
#define OP_BFEXTU_EA OP_UNDEFINED
#define OP_BFINS_DN  OP_UNDEFINED
#define OP_BFINS_EA  OP_UNDEFINED
#define OP_BFFFO_DN  OP_UNDEFINED
#define OP_BFFFO_EA  OP_UNDEFINED
#define OP_BFSET_DN  OP_UNDEFINED
#define OP_BFSET_EA  OP_UNDEFINED

// CHK.W: 68000 version (no CHK.L on 68000).  Unlike the 68030 path above,
// N is written only when the exception is taken and left alone in bounds --
// this is what the 68000 single-step corpus (CHK.json) passes against, and
// the two CPUs genuinely differ in the formally undefined case.
#define OP_CHK_W_EA_DN                                                                                                 \
    OP(                                                                                                                \
        VALID_EA(ea_data); LOAD_EA_WITH_UPDATE(16, src); int16_t dn = (int16_t)(uint16_t)DX;                           \
        int16_t bound = (int16_t)src; if (dn < 0) {                                                                    \
            CC_N = 1;                                                                                                  \
            EXC_CHK();                                                                                                 \
        } else if (dn > bound) {                                                                                       \
            CC_N = 0;                                                                                                  \
            EXC_CHK();                                                                                                 \
        })
#define OP_CHK_L_EA_DN         OP_UNDEFINED

// CHK2/CAS/CAS2/MOVES: undefined on 68000
#define OP_CHK2_B_EA_DN        OP_UNDEFINED
#define OP_CHK2_W_EA_DN        OP_UNDEFINED
#define OP_CHK2_L_EA_DN        OP_UNDEFINED
#define OP_CAS_B_DC_DU_EA      OP_UNDEFINED
#define OP_CAS_W_DC_DU_EA      OP_UNDEFINED
#define OP_CAS_L_DC_DU_EA      OP_UNDEFINED
#define OP_CAS2_W_DC_DU_RN     OP_UNDEFINED
#define OP_CAS2_L_DC_DU_RN     OP_UNDEFINED
#define OP_MOVES_B_RN_EA       OP_UNDEFINED
#define OP_MOVES_W_RN_EA       OP_UNDEFINED
#define OP_MOVES_L_RN_EA       OP_UNDEFINED
#define OP_MOVES_B_EA_RN       OP_UNDEFINED
#define OP_MOVES_W_EA_RN       OP_UNDEFINED
#define OP_MOVES_L_EA_RN       OP_UNDEFINED

// MOVEM reg->mem: 68000 version
#define OP_MOVEM_W_LIST_EA     OP(VALID_EA((ea_control & ea_alterable) | ea_min_an); MOVEM_FROM_REGISTER(opcode, 16))
#define OP_MOVEM_L_LIST_EA     OP(VALID_EA((ea_control & ea_alterable) | ea_min_an); MOVEM_FROM_REGISTER(opcode, 32))

// MOVE SR,<ea>: not privileged on 68000
#define OP_MOVE_W_SR_EA        OP(VALID_EA((ea_data & ea_alterable)); STORE_EA(16, GET_SR()))

// TST: 68000 restricted to data-alterable
#define OP_TST_B_EA            OP(VALID_EA((ea_data & ea_alterable)); LOAD_EA_WITH_UPDATE(8, ea); UPDATE_NZ_CLEAR_CV(ea))
#define OP_TST_W_EA            OP(VALID_EA((ea_data & ea_alterable)); LOAD_EA_WITH_UPDATE(16, ea); UPDATE_NZ_CLEAR_CV(ea))
#define OP_TST_L_EA            OP(VALID_EA((ea_data & ea_alterable)); LOAD_EA_WITH_UPDATE(32, ea); UPDATE_NZ_CLEAR_CV(ea))

// MULS.L/DIVS.L: undefined on 68000
#define OP_MULS_L_EA_DH_DL     OP_UNDEFINED
#define OP_DIVS_L_EA_DR_DQ     OP_UNDEFINED

// RTD: undefined on 68000
#define OP_RTD_DISPLACEMENT    OP_UNDEFINED

// MOVEC: undefined on 68000
#define OP_MOVEC_RC_RN         OP_UNDEFINED
#define OP_MOVEC_RN_RC         OP_UNDEFINED

// RTE: 68000 simple frame
#define OP_RTE                 OP(SUPER(uint16_t sr; POP16(sr); POP32(PC); SET_SR(sr)))

// LEA: 68000 version (no EXTB.L)
#define OP_LEA_EA_AN           OP(VALID_EA(ea_control); AX = GET_EA)

// TRAPcc: undefined on 68000
#define OP_TRAPCC              OP_UNDEFINED
#define OP_TRAPCC_W_DATA       OP_UNDEFINED
#define OP_TRAPCC_L_DATA       OP_UNDEFINED

// PACK/UNPK: undefined on 68000
#define OP_PACK_DY_DX          OP_UNDEFINED
#define OP_PACK_AY_AX          OP_UNDEFINED
#define OP_UNPK_DY_DX          OP_UNDEFINED
#define OP_UNPK_AY_AX          OP_UNDEFINED

// MOVE CCR,<ea>: undefined on 68000
#define OP_MOVE_B_CCR_EA       OP_UNDEFINED

// LINK.L: undefined on 68000
#define OP_LINK_L_AN_DISP      OP_UNDEFINED

// BKPT: undefined on 68000
#define OP_BKPT_DATA           OP_UNDEFINED

// PBCC/PSAVE/PRESTORE: F-line trap on 68000
#define OP_PBCC_W              OP(EXC_FTRAP())
#define OP_PBCC_L              OP(EXC_FTRAP())
#define OP_PSAVE_EA            OP(EXC_FTRAP())
#define OP_PRESTORE_EA         OP(EXC_FTRAP())

// CpID=0 type=0 general MMU instruction: F-line exception on 68000
#define OP_PMMU_GENERAL        OP(EXC_FTRAP())

// FPU: F-line trap on 68000
#define OP_FBCC_W_DISPLACEMENT OP(EXC_FTRAP())
#define OP_FBCC_L_DISPLACEMENT OP(EXC_FTRAP())
#define OP_FSAVE_EA            OP(EXC_FTRAP())
#define OP_FRESTORE_EA         OP(EXC_FTRAP())
#define OP_FPU_GENERAL         OP(EXC_FTRAP())
#define OP_FPU_SCCDBCC         OP(EXC_FTRAP())
#define OP_FTRAP               OP(EXC_FTRAP())

#endif // CPU_DECODER_IS_68030

#endif // CPU_OPS_H
