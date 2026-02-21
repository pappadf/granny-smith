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

// These variants will be needed for instructions like MOVEP
#define READ2x8(addr)     ((uint16_t)READ8(addr) << 8 | (uint16_t)READ8(addr + 2))
#define READ4x8(addr)     ((uint32_t)READ2x8(addr) << 16 | (uint32_t)READ2x8(addr + 4))
#define WRITE2x8(addr, x) (WRITE8(addr, (x) >> 8 & 0xFF), WRITE8(addr + 2, (x) & 0xFF))
#define WRITE4x8(addr, x) (WRITE2x8(addr, (x) >> 16 & 0xFFFF), WRITE2x8(addr + 4, (x) & 0xFFFF))

#define DISP8()  ((int32_t)(int8_t)(opcode & 0xFF))
#define DISP16() ((int32_t)(int16_t)FETCH16())
#define DISP32() ((int32_t)FETCH32())

// We use the term "load" when reading data into a new variable
#define LOAD_IMM(bits, var) UINT(bits) var = FETCH##bits()

// Helper macros to extract bit fields from opcode
#define EA_MODE ((opcode >> 3) & 7)
#define EA_REG  (opcode & 7)
#define DATA    ((((opcode >> 9 & 7) - 1) & 7) + 1)

#define VALIDATE_EA(supported_modes, mode, reg)                                                                        \
    if (!((supported_modes) & 1u << ((mode) + ((mode) == 7 ? (reg) : 0)))) {                                           \
        EXC_ILLEGAL();                                                                                                 \
        continue;                                                                                                      \
    }
#define VALID_EA(modes)      VALIDATE_EA(modes, EA_MODE, EA_REG)
#define VALID_EA_MOVE(modes) VALIDATE_EA(modes, (opcode >> 6) & 7, (opcode >> 9) & 7)

#define LOAD_EA(bits, x, modes)                                                                                        \
    VALID_EA(modes);                                                                                                   \
    UINT(bits) x = READ_EA(bits, opcode, false)
#define LOAD_EA_WITH_UPDATE(bits, x) UINT(bits) x = READ_EA(bits, opcode, true)

// load from -(An), i.e. An in pre-decrement mode
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
#define CLEAR_N()    CC_N = 0
#define CLEAR_NZVC() CC_N = CC_Z = CC_V = CC_C = 0

// Generic helper macros to update condition codes based on result
#define UPDATE_N(res)           CC_N = res & 1u << (BITS(res) - 1)
#define UPDATE_Z(res)           CC_Z = !(res)
#define UPDATE_NZ_CLEAR_V(res)  UPDATE_N(res), UPDATE_Z(res), CC_V = 0
#define UPDATE_NZ_CLEAR_CV(res) UPDATE_N(res), UPDATE_Z(res), CC_C = CC_V = 0

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

// Generic SUB
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
    UINT(bits) src = DN;                                                                                               \
    SUB(bits, dst, (UINT(bits))DN, res);                                                                               \
    STORE_EA(bits, res);

#define SUBI(bits)                                                                                                     \
    VALID_EA((ea_data & ea_alterable));                                                                                \
    LOAD_IMM(bits, src);                                                                                               \
    LOAD_EA(bits, dst, (ea_data & ea_alterable));                                                                      \
    SUB(bits, dst, src, res);                                                                                          \
    STORE_EA(bits, res);

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

// SUBX.[BWL] -(Ax),-(Ay)
#define SUBX_AX_AY(bits)                                                                                               \
    LOAD_AN_PREDEC(bits, src, EA_REG);                                                                                 \
    LOAD_AN_PREDEC(bits, dst, opcode >> 9 & 7);                                                                        \
    UINT(bits) res;                                                                                                    \
    SUBX(dst, src, res);                                                                                               \
    STORE_AT_AN(bits, opcode >> 9 & 7, res);

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

#define CMPM_AY_AX(bits)                                                                                               \
    LOAD_AN_POSTINC(bits, src, EA_REG);                                                                                \
    LOAD_AN_POSTINC(bits, dst, opcode >> 9 & 7);                                                                       \
    UINT(bits) res;                                                                                                    \
    GENERIC_SUB(dst, src, res);

// ORI/ANDI/EORI.[BWL] #<data>,<ea>
#define IMM_LOGICAL(bits, op)                                                                                          \
    VALID_EA((ea_data & ea_alterable));                                                                                \
    LOAD_IMM(bits, src);                                                                                               \
    LOAD_EA(bits, dst, (ea_data & ea_alterable));                                                                      \
    UINT(bits) res = dst op src;                                                                                       \
    UPDATE_NZ_CLEAR_CV(res);                                                                                           \
    STORE_EA(bits, res);

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

// ADDI.[BWL] #<data>,<ea>
#define ADDI(bits)                                                                                                     \
    VALID_EA((ea_data & ea_alterable));                                                                                \
    LOAD_IMM(bits, src);                                                                                               \
    LOAD_EA(bits, dst, (ea_data & ea_alterable));                                                                      \
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

#define ADDX_AX_AY(bits)                                                                                               \
    LOAD_AN_PREDEC(bits, src, EA_REG);                                                                                 \
    LOAD_AN_PREDEC(bits, dst, opcode >> 9 & 7);                                                                        \
    UINT(bits) res;                                                                                                    \
    ADDX(dst, src, res);                                                                                               \
    STORE_AT_AN(bits, opcode >> 9 & 7, res);

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

#define XBCD_AY_AX(op)                                                                                                 \
    LOAD_AN8_PREDEC(src, EA_REG);                                                                                      \
    LOAD_AN8_PREDEC(dst, opcode >> 9 & 7);                                                                             \
    uint8_t res = op(dst, src);                                                                                        \
    STORE_AT_AN(8, opcode >> 9 & 7, res);

#define XBCD_DY_DX(op) STORE_DN(8, opcode >> 9 & 7, op(DX, DY));

// CMPI.[BWL] #<data>,<ea>
#define CMPI(bits)                                                                                                     \
    VALID_EA(ea_data - ea_xxx - ea_d16_pc - ea_d8_pc_xn);                                                              \
    LOAD_IMM(bits, src);                                                                                               \
    LOAD_EA_WITH_UPDATE(bits, dst);                                                                                    \
    uint##bits##_t res;                                                                                                \
    GENERIC_SUB(dst, src, res);

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

#define ASHIFT_LEFT(bits, data, count, op)                                                                             \
    SHIFT_COMMON(bits, data, count, op);                                                                               \
    UPDATE_C_SHIFT_L(d, c);                                                                                            \
    UPDATE_X_SHIFT(c);                                                                                                 \
    UPDATE_N(r);                                                                                                       \
    UPDATE_Z(r);                                                                                                       \
    CC_V = !r && d || (UINT(bits))((INT(bits))(1u << (bits - 1) & d) >> c ^ d) >> (bits - c - 1);

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
        uint32_t quotient = DN / (uint16_t)divisor;                                                                    \
        if (quotient > UINT16_MAX) {                                                                                   \
            CC_V = CC_N = 1;                                                                                           \
        } else {                                                                                                       \
            uint32_t remainder = DN % (uint16_t)divisor;                                                               \
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
    } else {                                                                                                           \
        int32_t q = (INT(32))DN / (int16_t)divisor;                                                                    \
        if (((int16_t)divisor == -1 && (INT(32))DN == INT32_MIN) || q > INT16_MAX || q < INT16_MIN) {                  \
            CC_V = CC_N = 1;                                                                                           \
        } else {                                                                                                       \
            int32_t remainder = (INT(32))DN % (int16_t)divisor;                                                        \
            DX = ((uint32_t)remainder << 16) | ((uint32_t)q & 0xFFFF);                                                 \
            CC_N = q & 0x8000;                                                                                         \
            CC_Z = (q == 0);                                                                                           \
        }                                                                                                              \
    }

// Generic bit operation helpers - all bit manipulation instructions follow the same pattern
// with different operations (XOR, AND-NOT, OR) and optional write-back
#define BIT_OP_WRITE(size, bit, operation)                                                                             \
    VALID_EA((ea_data & ea_alterable));                                                                                \
    UINT(size) mask = 1u << (bit);                                                                                     \
    LOAD_EA(size, dst, (ea_data & ea_alterable));                                                                      \
    CC_Z = !(dst & mask);                                                                                              \
    STORE_EA(size, operation);

#define BIT_OP_TEST(size, bit, mode)                                                                                   \
    VALID_EA(mode);                                                                                                    \
    UINT(size) mask = 1u << (bit);                                                                                     \
    LOAD_EA_WITH_UPDATE(size, dst);                                                                                    \
    CC_Z = !(dst & mask);

#define MOVE(size)                                                                                                     \
    VALID_EA(ea_any - ea_an);                                                                                          \
    LOAD_EA_WITH_UPDATE(size, src);                                                                                    \
    WRITE_EA(size, opcode >> 6 & 7, opcode >> 9 & 7, src);                                                             \
    UPDATE_NZ_CLEAR_CV(src);

#define MOVEx(size)                                                                                                    \
    VALID_EA(ea_any);                                                                                                  \
    LOAD_EA_WITH_UPDATE(size, src);                                                                                    \
    WRITE_EA(size, opcode >> 6 & 7, opcode >> 9 & 7, src);                                                             \
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
        int16_t counter = DY - 1;                                                                                      \
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

#define PUSH(x)                                                                                                        \
    SP -= 4;                                                                                                           \
    WRITE32(SP, (x));
#define PUSH32(x)                                                                                                      \
    SP -= 4;                                                                                                           \
    WRITE32(SP, (x));

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
#define OP_MOVE_B_EA_EA     OP(VALID_EA_MOVE((ea_data & ea_alterable)); MOVE(8))
#define OP_MOVE_W_EA_EA     OP(VALID_EA_MOVE((ea_data & ea_alterable)); MOVEx(16))
#define OP_MOVE_L_EA_EA     OP(VALID_EA_MOVE((ea_data & ea_alterable)); MOVEx(32))
#define OP_BFTST_DN         OP_UNDEFINED
#define OP_BFTST_EA         OP_UNDEFINED
#define OP_BFCHG_DN         OP_UNDEFINED
#define OP_BFCHG_EA         OP_UNDEFINED
#define OP_BFCLR_DN         OP_UNDEFINED
#define OP_BFCLR_EA         OP_UNDEFINED
#define OP_BFEXTS_DN        OP_UNDEFINED
#define OP_BFEXTS_EA        OP_UNDEFINED
#define OP_BFEXTU_DN        OP_UNDEFINED
#define OP_BFEXTU_EA        OP_UNDEFINED
#define OP_BFINS_DN         OP_UNDEFINED
#define OP_BFINS_EA         OP_UNDEFINED
#define OP_BFFFO_DN         OP_UNDEFINED
#define OP_BFFFO_EA         OP_UNDEFINED
#define OP_BFSET_DN         OP_UNDEFINED
#define OP_BFSET_EA         OP_UNDEFINED
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
#define OP_PEA_EA              OP(VALID_EA(ea_control); uint32_t ea = GET_EA; PUSH(ea))
#define OP_BSR_B_LABEL         OP(uint32_t p = PC + DISP8(); PUSH(PC); PC = p)
#define OP_BSR_W_LABEL         OP(uint32_t p = PC + DISP16(); PUSH(PC); PC = p)
#define OP_BSR_L_LABEL         OP(uint32_t p = PC + DISP32(); PUSH(PC); PC = p)
#define OP_ORI_B_DATA_CCR      OP(TO_CCR(|))
#define OP_ORI_W_DATA_SR       OP(TO_SR(|))
#define OP_ORI_B_DATA_EA       OP(IMM_LOGICAL(8, |))
#define OP_ORI_W_DATA_EA       OP(IMM_LOGICAL(16, |))
#define OP_ORI_L_DATA_EA       OP(IMM_LOGICAL(32, |))
#define OP_ANDI_B_DATA_CCR     OP(TO_CCR(&))
#define OP_ANDI_W_DATA_SR      OP(TO_SR(&))
#define OP_ANDI_B_DATA_EA      OP(IMM_LOGICAL(8, &))
#define OP_ANDI_W_DATA_EA      OP(IMM_LOGICAL(16, &))
#define OP_ANDI_L_DATA_EA      OP(IMM_LOGICAL(32, &))
#define OP_CHK2_B_EA_DN        OP_UNDEFINED
#define OP_CHK2_W_EA_DN        OP_UNDEFINED
#define OP_CHK2_L_EA_DN        OP_UNDEFINED
#define OP_RTM_RN              OP_UNDEFINED
#define OP_CALLM_DATA_EA       OP_UNDEFINED
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
#define OP_SUBI_B_DATA_EA      OP(SUBI(8))
#define OP_SUBI_W_DATA_EA      OP(SUBI(16))
#define OP_SUBI_L_DATA_EA      OP(SUBI(32))
#define OP_ADDI_B_DATA_EA      OP(ADDI(8))
#define OP_ADDI_W_DATA_EA      OP(ADDI(16))
#define OP_ADDI_L_DATA_EA      OP(ADDI(32))
#define OP_EORI_B_DATA_CCR     OP(TO_CCR(^))
#define OP_EORI_W_DATA_SR      OP(TO_SR(^))
#define OP_EORI_B_DATA_EA      OP(IMM_LOGICAL(8, ^))
#define OP_EORI_W_DATA_EA      OP(IMM_LOGICAL(16, ^))
#define OP_EORI_L_DATA_EA      OP(IMM_LOGICAL(32, ^))
#define OP_CMPI_B_DATA_EA      OP(CMPI(8))
#define OP_CMPI_W_DATA_EA      OP(CMPI(16))
#define OP_CMPI_L_DATA_EA      OP(CMPI(32))
#define OP_NEGX_B_EA           OP(NEGX(8))
#define OP_NEGX_W_EA           OP(NEGX(16))
#define OP_NEGX_L_EA           OP(NEGX(32))
#define OP_MOVE_W_SR_EA        OP(VALID_EA((ea_data & ea_alterable)); STORE_EA(16, GET_SR()))
#define OP_NEG_B_EA            OP(NEG(8))
#define OP_NEG_W_EA            OP(NEG(16))
#define OP_NEG_L_EA            OP(NEG(32))
#define OP_MOVE_B_CCR_EA       OP_UNDEFINED
#define OP_MOVE_B_EA_CCR       OP(VALID_EA(ea_data); LOAD_EA_WITH_UPDATE(16, src); WRITE_CCR(src))
#define OP_LINK_L_AN_DISP      OP_UNDEFINED
#define OP_NBCD_B_EA           OP(LOAD_EA(8, src, (ea_data & ea_alterable)); STORE_EA(8, SBCD(0, src)))
#define OP_BKPT_DATA           OP_UNDEFINED
#define OP_EXT_W_DN            OP(UINT(16) r = S_EXT_8TO16(DY); STORE_DN(16, EA_REG, r); UPDATE_NZ_CLEAR_CV(r))
#define OP_EXT_L_DN            OP(UINT(32) r = S_EXT_16TO32(DY); DY = r; UPDATE_NZ_CLEAR_CV(r))
#define OP_MOVEM_W_LIST_EA     OP(VALID_EA((ea_control & ea_alterable) | ea_min_an); MOVEM_FROM_REGISTER(opcode, 16))
#define OP_MOVEM_L_LIST_EA     OP(VALID_EA((ea_control & ea_alterable) | ea_min_an); MOVEM_FROM_REGISTER(opcode, 32))
#define OP_TST_B_EA            OP(VALID_EA((ea_data & ea_alterable)); LOAD_EA_WITH_UPDATE(8, ea); UPDATE_NZ_CLEAR_CV(ea))
#define OP_TST_W_EA            OP(VALID_EA((ea_data & ea_alterable)); LOAD_EA_WITH_UPDATE(16, ea); UPDATE_NZ_CLEAR_CV(ea))
#define OP_TST_L_EA            OP(VALID_EA((ea_data & ea_alterable)); LOAD_EA_WITH_UPDATE(32, ea); UPDATE_NZ_CLEAR_CV(ea))
#define OP_UNDEFINED           OP(EXC_ILLEGAL(); continue)
#define OP_ILLEGAL             OP_UNDEFINED
#define OP_TAS_B_EA            OP(LOAD_EA(8, ea, (ea_data & ea_alterable)); UPDATE_NZ_CLEAR_CV(ea); ea |= 0x80; STORE_EA(8, ea))
#define OP_MULS_L_EA_DH_DL     OP_UNDEFINED
#define OP_DIVS_L_EA_DR_DQ     OP_UNDEFINED
#define OP_MOVEM_W_EA_LIST     OP(VALID_EA(ea_control + ea_an_plus); MOVEM_TO_REGISTER(opcode, 16))
#define OP_MOVEM_L_EA_LIST     OP(VALID_EA(ea_control + ea_an_plus); MOVEM_TO_REGISTER(opcode, 32))
#define OP_TRAP_VECTOR         OP(EXC_TRAP(opcode & 0xF))
#define OP_RESET               OP(SUPER())
#define OP_STOP_DATA           OP(SUPER(uint16_t sr = FETCH16(); SET_SR(sr)))
#define OP_RTD_DISPLACEMENT    OP_UNDEFINED
#define OP_RTS                 OP(POP32(PC))
#define OP_TRAPV               OP(if (CC_V) EXC_TRAPV())
#define OP_RTE                 OP(SUPER(uint16_t sr; POP16(sr); POP32(PC); SET_SR(sr)))
#define OP_RTR                 OP(uint16_t ccr; POP16(ccr); WRITE_CCR(ccr); POP32(PC))
#define OP_MOVEC_RC_RN         OP_UNDEFINED
#define OP_MOVEC_RN_RC         OP_UNDEFINED
#define OP_LINK                OP(uint32_t a = AY; PUSH32(a); AY = SP; SP += (int32_t)(int16_t)FETCH16())
#define OP_UNLK                OP(SP = AY; uint32_t a; POP32(a); AY = a)
#define OP_NOP                 OP(/* no-op */)
#define OP_MOVE_AN_USP         OP(SUPER(SET_USP(A(EA_REG))))
#define OP_MOVE_USP_AN         OP(SUPER(A(EA_REG) = GET_USP()))
#define OP_LEA_EA_AN           OP(VALID_EA(ea_control); AX = GET_EA)
#define OP_SCC_EA              OP(VALID_EA((ea_data & ea_alterable)); STORE_EA(8, CC ? 0xFF : 0))
#define OP_TRAPCC              OP_UNDEFINED
#define OP_TRAPCC_W_DATA       OP_UNDEFINED
#define OP_TRAPCC_L_DATA       OP_UNDEFINED
#define OP_DBCC_DN_LABEL       OP(DBCC_DN_LABEL)
#define OP_SUBQ_W_DATA_AN      OP(AY -= DATA)
#define OP_SUBQ_L_DATA_AN      OP(AY -= DATA)
#define OP_SUBQ_B_DATA_EA      OP(SUBQ(8))
#define OP_SUBQ_W_DATA_EA      OP(SUBQ(16))
#define OP_SUBQ_L_DATA_EA      OP(SUBQ(32))
#define OP_ADDQ_W_DATA_AN      OP(AY += DATA)
#define OP_ADDQ_L_DATA_AN      OP(AY += DATA)
#define OP_ADDQ_B_DATA_EA      OP(ADDQ(8))
#define OP_ADDQ_W_DATA_EA      OP(ADDQ(16))
#define OP_ADDQ_L_DATA_EA      OP(ADDQ(32))
#define OP_BCC_L_DISPLACEMENT  OP(PC += CC ? (int32_t)FETCH32_NO_INC() : 4)
#define OP_BCC_W_DISPLACEMENT  OP(PC += CC ? (int32_t)(int16_t)FETCH16_NO_INC() : 2)
#define OP_BCC_B_DISPLACEMENT  OP(PC += CC ? (int32_t)(int8_t)(opcode & 0xFF) : 0)
#define OP_MOVEQ_L_DATA_DN     OP(uint32_t res = (int32_t)(int8_t)opcode; DX = res; UPDATE_NZ_CLEAR_CV(res))
#define OP_SBCD_DX_DY          OP(XBCD_DY_DX(SBCD))
#define OP_SBCD_AX_AY          OP(XBCD_AY_AX(SBCD))
#define OP_PACK_DY_DX          OP_UNDEFINED
#define OP_PACK_AY_AX          OP_UNDEFINED
#define OP_UNPK_DY_DX          OP_UNDEFINED
#define OP_UNPK_AY_AX          OP_UNDEFINED
#define OP_OR_B_EA_DN          OP(LOGICAL_EA_DN(8, |, ea_data))
#define OP_OR_W_EA_DN          OP(LOGICAL_EA_DN(16, |, ea_data))
#define OP_OR_L_EA_DN          OP(LOGICAL_EA_DN(32, |, ea_data))
#define OP_OR_B_DN_EA          OP(LOGICAL_DN_EA(8, |, (ea_memory & ea_alterable)))
#define OP_OR_W_DN_EA          OP(LOGICAL_DN_EA(16, |, (ea_memory & ea_alterable)))
#define OP_OR_L_DN_EA          OP(LOGICAL_DN_EA(32, |, (ea_memory & ea_alterable)))
#define OP_DIVU_W_EA_DN        OP(DIV16U)
#define OP_DIVS_W_EA_DN        OP(DIV16S)
#define OP_SUBA_W_EA_AN        OP(VALID_EA(ea_any); LOAD_EA_WITH_UPDATE(16, src); AN -= (int32_t)(int16_t)src)
#define OP_SUBA_L_EA_AN        OP(VALID_EA(ea_any); LOAD_EA_WITH_UPDATE(32, src); AN -= src)
#define OP_SUBX_B_DX_DY        OP(SUBX_DX_DY(8))
#define OP_SUBX_W_DX_DY        OP(SUBX_DX_DY(16))
#define OP_SUBX_L_DX_DY        OP(SUBX_DX_DY(32))
#define OP_SUBX_B_AX_AY        OP(SUBX_AX_AY(8))
#define OP_SUBX_W_AX_AY        OP(SUBX_AX_AY(16))
#define OP_SUBX_L_AX_AY        OP(SUBX_AX_AY(32))
#define OP_SUB_B_EA_DN         OP(SUB_EA_DN(8, ea_any - ea_an))
#define OP_SUB_W_EA_DN         OP(SUB_EA_DN(16, ea_any))
#define OP_SUB_L_EA_DN         OP(SUB_EA_DN(32, ea_any))
#define OP_SUB_B_DN_EA         OP(SUB_DN_EA(8))
#define OP_SUB_W_DN_EA         OP(SUB_DN_EA(16))
#define OP_SUB_L_DN_EA         OP(SUB_DN_EA(32))
#define OP_ATRAP               OP(EXC_ATRAP())
#define OP_CMPM_B_AY_AX        OP(CMPM_AY_AX(8))
#define OP_CMPM_W_AY_AX        OP(CMPM_AY_AX(16))
#define OP_CMPM_L_AY_AX        OP(CMPM_AY_AX(32))
#define OP_CMPA_W_EA_AN        OP(CMPA_EA_AN(16))
#define OP_CMPA_L_EA_AN        OP(CMPA_EA_AN(32))
#define OP_EOR_B_DN_EA         OP(LOGICAL_DN_EA(8, ^, (ea_data & ea_alterable)))
#define OP_EOR_W_DN_EA         OP(LOGICAL_DN_EA(16, ^, (ea_data & ea_alterable)))
#define OP_EOR_L_DN_EA         OP(LOGICAL_DN_EA(32, ^, (ea_data & ea_alterable)))
#define OP_CMP_B_EA_DN         OP(CMP_EA_DN(8, ea_any - ea_an))
#define OP_CMP_W_EA_DN         OP(CMP_EA_DN(16, ea_any))
#define OP_CMP_L_EA_DN         OP(CMP_EA_DN(32, ea_any))
#define OP_AND_B_EA_DN         OP(LOGICAL_EA_DN(8, &, ea_data))
#define OP_AND_W_EA_DN         OP(LOGICAL_EA_DN(16, &, ea_data))
#define OP_AND_L_EA_DN         OP(LOGICAL_EA_DN(32, &, ea_data))
#define OP_MULU_W_EA_DN        OP(MUL_W(uint))
#define OP_ABCD_DY_DX          OP(XBCD_DY_DX(ABCD))
#define OP_ABCD_AY_AX          OP(XBCD_AY_AX(ABCD))
#define OP_AND_B_DN_EA         OP(LOGICAL_DN_EA(8, &, (ea_memory & ea_alterable)))
#define OP_EXG_DX_DY           OP(EXG(DX, DY))
#define OP_EXG_AX_AY           OP(EXG(AX, AY))
#define OP_AND_W_DN_EA         OP(LOGICAL_DN_EA(16, &, (ea_memory & ea_alterable)))
#define OP_EXG_DX_AY           OP(EXG(DX, AY))
#define OP_AND_L_DN_EA         OP(LOGICAL_DN_EA(32, &, (ea_memory & ea_alterable)))
#define OP_MULS_W_EA_DN        OP(MUL_W(int))
#define OP_ADD_B_EA_DN         OP(ADD_EA_DN(8, ea_any - ea_an))
#define OP_ADD_W_EA_DN         OP(ADD_EA_DN(16, ea_any))
#define OP_ADD_L_EA_DN         OP(ADD_EA_DN(32, ea_any))
#define OP_ADDA_W_EA_AN        OP(VALID_EA(ea_any); LOAD_EA_WITH_UPDATE(16, src); AN += (int32_t)(int16_t)src)
#define OP_ADDX_B_DY_DX        OP(ADDX_DX_DY(8))
#define OP_ADDX_B_AY_AX        OP(ADDX_AX_AY(8))
#define OP_ADD_B_DN_EA         OP(ADD_DN_EA(8))
#define OP_ADDX_W_DY_DX        OP(ADDX_DX_DY(16))
#define OP_ADDX_W_AY_AX        OP(ADDX_AX_AY(16))
#define OP_ADD_W_DN_EA         OP(ADD_DN_EA(16))
#define OP_ADDX_L_DY_DX        OP(ADDX_DX_DY(32))
#define OP_ADDX_L_AY_AX        OP(ADDX_AX_AY(32))
#define OP_ADD_L_DN_EA         OP(ADD_DN_EA(32))
#define OP_ADDA_L_EA_AN        OP(VALID_EA(ea_any); LOAD_EA_WITH_UPDATE(32, src); AN += src)
#define OP_ASR_B_DATA_DY       OP(SHIFT_RIGHT(8, DY, DATA, (int8_t)d >> MIN(c, 7)))
#define OP_LSR_B_DATA_DY       OP(SHIFT_RIGHT(8, DY, DATA, c > 7 ? 0 : d >> c))
#define OP_ROXR_B_DATA_DY      OP(ROXR(8, DY, DATA))
#define OP_ROR_B_DATA_DY       OP(ROTATE_RIGHT(8, DY, DATA))
#define OP_ASR_B_DX_DY         OP(SHIFT_RIGHT(8, DY, DX & 0x3F, (int8_t)d >> MIN(c, 7)))
#define OP_LSR_B_DX_DY         OP(SHIFT_RIGHT(8, DY, DX & 0x3F, c > 7 ? 0 : d >> c))
#define OP_ROXR_B_DX_DY        OP(ROXR(8, DY, (DX & 0x3F) % 9))
#define OP_ROR_B_DX_DY         OP(ROTATE_RIGHT(8, DY, DX & 0x3F))
#define OP_ASR_W_DATA_DY       OP(SHIFT_RIGHT(16, DY, DATA, (int16_t)d >> c))
#define OP_LSR_W_DATA_DY       OP(SHIFT_RIGHT(16, DY, DATA, d >> c))
#define OP_ROXR_W_DATA_DY      OP(ROXR(16, DY, DATA))
#define OP_ROR_W_DATA_DY       OP(ROTATE_RIGHT(16, DY, DATA))
#define OP_ASR_W_DX_DY         OP(SHIFT_RIGHT(16, DY, DX & 0x3F, (int16_t)d >> MIN(c, 15)))
#define OP_LSR_W_DX_DY         OP(SHIFT_RIGHT(16, DY, D(opcode >> 9 & 7) & 0x3F, c > 15 ? 0 : d >> c))
#define OP_ROXR_W_DX_DY        OP(ROXR(16, DY, (DX & 0x3F) % 17))
#define OP_ROR_W_DX_DY         OP(ROTATE_RIGHT(16, DY, DX & 0x3F))
#define OP_ASR_L_DATA_DY       OP(SHIFT_RIGHT(32, DY, DATA, (int32_t)d >> c))
#define OP_LSR_L_DATA_DY       OP(SHIFT_RIGHT(32, DY, DATA, d >> c))
#define OP_ROXR_L_DATA_DY      OP(ROXR(32, DY, DATA))
#define OP_ROR_L_DATA_DY       OP(ROTATE_RIGHT(32, DY, DATA))
#define OP_ASR_L_DX_DY         OP(SHIFT_RIGHT(32, DY, D(opcode >> 9 & 7) & 0x3F, (int32_t)d >> MIN(c, 31)))
#define OP_LSR_L_DX_DY         OP(SHIFT_RIGHT(32, DY, D(opcode >> 9 & 7) & 0x3F, c > 31 ? 0 : d >> c))
#define OP_ROXR_L_DX_DY        OP(ROXR(32, DY, (DX & 0x3F) % 33))
#define OP_ROR_L_DX_DY         OP(ROTATE_RIGHT(32, DY, DX & 0x3F))
#define OP_ASR_W_EA            OP(SHIFT_EA_R((int16_t)ea >> 1); CC_X = ea & 1)
#define OP_LSR_W_EA            OP(SHIFT_EA_R(ea >> 1); CLEAR_N(); CC_X = ea & 1)
#define OP_ROXR_W_EA           OP(SHIFT_EA_R(ea >> 1 | (CC_X ? 0x8000 : 0)); CC_X = ea & 1)
#define OP_ROR_W_EA            OP(SHIFT_EA_R(ea >> 1 | ea << 15))
#define OP_ASL_B_DATA_DY       OP(ASHIFT_LEFT(8, DY, DATA, c < 8 ? d << c : 0))
#define OP_LSL_B_DATA_DY       OP(LSHIFT_LEFT(8, DY, DATA, c < 8 ? d << c : 0))
#define OP_ROXL_B_DATA_DY      OP(ROXL(8, DY, DATA))
#define OP_ROL_B_DATA_DY       OP(ROTATE_LEFT(8, DY, DATA))
#define OP_ASL_B_DX_DY         OP(ASHIFT_LEFT(8, DY, DX & 0x3F, c < 8 ? d << c : 0))
#define OP_LSL_B_DX_DY         OP(LSHIFT_LEFT(8, DY, DX & 0x3F, c < 8 ? d << c : 0))
#define OP_ROXL_B_DX_DY        OP(ROXL(8, DY, (DX & 0x3F) % 9))
#define OP_ROL_B_DX_DY         OP(ROTATE_LEFT(8, DY, DX & 0x3F))
#define OP_ASL_W_DATA_DY       OP(ASHIFT_LEFT(16, DY, DATA, d << c))
#define OP_LSL_W_DATA_DY       OP(LSHIFT_LEFT(16, DY, DATA, d << c))
#define OP_ROXL_W_DATA_DY      OP(ROXL(16, DY, DATA))
#define OP_ROL_W_DATA_DY       OP(ROTATE_LEFT(16, DY, DATA))
#define OP_ASL_W_DX_DY         OP(ASHIFT_LEFT(16, DY, DX & 0x3F, c < 16 ? d << c : 0))
#define OP_LSL_W_DX_DY         OP(LSHIFT_LEFT(16, DY, DX & 0x3F, c < 16 ? d << c : 0))
#define OP_ROXL_W_DX_DY        OP(ROXL(16, DY, (DX & 0x3F) % 17))
#define OP_ROL_W_DX_DY         OP(ROTATE_LEFT(16, DY, DX & 0x3F))
#define OP_ASL_L_DATA_DY       OP(ASHIFT_LEFT(32, DY, DATA, d << c))
#define OP_LSL_L_DATA_DY       OP(LSHIFT_LEFT(32, DY, DATA, d << c))
#define OP_ROXL_L_DATA_DY      OP(ROXL(32, DY, DATA))
#define OP_ROL_L_DATA_DY       OP(ROTATE_LEFT(32, DY, DATA))
#define OP_ASL_L_DX_DY         OP(ASHIFT_LEFT(32, DY, DX & 0x3F, c < 32 ? d << c : 0))
#define OP_LSL_L_DX_DY         OP(LSHIFT_LEFT(32, DY, DX & 0x3F, c < 32 ? d << c : 0))
#define OP_ROXL_L_DX_DY        OP(ROXL(32, DY, (DX & 0x3F) % 33))
#define OP_ROL_L_DX_DY         OP(ROTATE_LEFT(32, DY, DX & 0x3F))
#define OP_ASL_W_EA            OP(SHIFT_EA_L(ea << 1); CC_X = CC_C; CC_V = (ea ^ ea << 1) & 0x8000)
#define OP_LSL_W_EA            OP(SHIFT_EA_L(ea << 1); CC_X = CC_C)
#define OP_ROXL_W_EA           OP(SHIFT_EA_L(ea << 1 | (CC_X ? 1 : 0)); CC_X = CC_C)
#define OP_ROL_W_EA            OP(SHIFT_EA_L(ea << 1 | ea >> 15))
#define OP_PBCC_W              OP(EXC_FTRAP())
#define OP_PBCC_L              OP(EXC_FTRAP())
#define OP_PSAVE_EA            OP(EXC_FTRAP())
#define OP_PRESTORE_EA         OP(EXC_FTRAP())
#define OP_FBCC_W_DISPLACEMENT OP(EXC_FTRAP())
#define OP_FBCC_L_DISPLACEMENT OP(EXC_FTRAP())
#define OP_FSAVE_EA            OP(EXC_FTRAP())
#define OP_FRESTORE_EA         OP(EXC_FTRAP())
#define OP_CINVL_CACHES_AN     OP(EXC_FTRAP())
#define OP_CINVP_CACHES_AN     OP(EXC_FTRAP())
#define OP_CINVA_CACHES        OP(EXC_FTRAP())
#define OP_CPUSHL_CACHES_AN    OP(EXC_FTRAP())
#define OP_CPUSHP_CACHES_AN    OP(EXC_FTRAP())
#define OP_CPUSHA_CACHES       OP(EXC_FTRAP())
#define OP_PFLUSH_AN           OP(EXC_FTRAP())
#define OP_PFLUSHN_AN          OP(EXC_FTRAP())
#define OP_PFLUSHA             OP(EXC_FTRAP())
#define OP_PFLUSHAN            OP(EXC_FTRAP())
#define OP_PTESTR_AN           OP(EXC_FTRAP())
#define OP_PTESTW_AN           OP(EXC_FTRAP())
#define OP_MOVE16_AN_P_XXX_L   OP(EXC_FTRAP())
#define OP_MOVE16_XXX_L_AN_P   OP(EXC_FTRAP())
#define OP_MOVE16_AN_XXX_L     OP(EXC_FTRAP())
#define OP_MOVE16_XXX_L_AN     OP(EXC_FTRAP())
#define OP_FTRAP               OP(EXC_FTRAP())

// ============================================================
// 68030-specific instruction overrides
// Re-define instruction macros that differ on the 68030.
// ============================================================
#ifdef CPU_DECODER_IS_68030

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
        int32_t _bo = (bf_offset);                                                                                     \
        uint32_t _w = (uint32_t)(w);                                                                                   \
        uint32_t _ea = (uint32_t)(ea);                                                                                 \
        /* Adjust for negative offset */                                                                               \
        if (_bo < 0) {                                                                                                 \
            int32_t _adj = ((-_bo + 7) >> 3) << 3;                                                                     \
            _ea -= (uint32_t)(_adj >> 3);                                                                              \
            _bo += _adj;                                                                                               \
        }                                                                                                              \
        uint32_t _byte_off = (uint32_t)_bo >> 3;                                                                       \
        uint32_t _bit_in = (uint32_t)_bo & 7u;                                                                         \
        uint32_t _n = (_bit_in + _w + 7u) >> 3;                                                                        \
        uint64_t _data = 0;                                                                                            \
        for (uint32_t _i = 0; _i < _n && _i < 5; _i++)                                                                 \
            _data = (_data << 8) | (uint8_t)READ8(_ea + _byte_off + _i);                                               \
        uint32_t _shift = _n * 8u - _bit_in - _w;                                                                      \
        uint32_t _mask = (_w == 32u) ? 0xFFFFFFFFu : ((1u << _w) - 1u);                                                \
        (result) = (uint32_t)((_data >> _shift) & _mask);                                                              \
    } while (0)

// Write bit field to memory using WRITE8 macro.
#define BF_INSERT_MEM(ea, bf_offset, w, value)                                                                         \
    do {                                                                                                               \
        int32_t _bo = (bf_offset);                                                                                     \
        uint32_t _w = (uint32_t)(w);                                                                                   \
        uint32_t _ea = (uint32_t)(ea);                                                                                 \
        uint32_t _val = (uint32_t)(value);                                                                             \
        if (_bo < 0) {                                                                                                 \
            int32_t _adj = ((-_bo + 7) >> 3) << 3;                                                                     \
            _ea -= (uint32_t)(_adj >> 3);                                                                              \
            _bo += _adj;                                                                                               \
        }                                                                                                              \
        uint32_t _byte_off = (uint32_t)_bo >> 3;                                                                       \
        uint32_t _bit_in = (uint32_t)_bo & 7u;                                                                         \
        uint32_t _n = (_bit_in + _w + 7u) >> 3;                                                                        \
        uint8_t _bytes[5] = {0, 0, 0, 0, 0};                                                                           \
        for (uint32_t _i = 0; _i < _n && _i < 5; _i++)                                                                 \
            _bytes[_i] = (uint8_t)READ8(_ea + _byte_off + _i);                                                         \
        uint64_t _data = 0;                                                                                            \
        for (uint32_t _i = 0; _i < _n; _i++)                                                                           \
            _data = (_data << 8) | _bytes[_i];                                                                         \
        uint32_t _mask = (_w == 32u) ? 0xFFFFFFFFu : ((1u << _w) - 1u);                                                \
        uint32_t _shift = _n * 8u - _bit_in - _w;                                                                      \
        _data = (_data & ~((uint64_t)_mask << _shift)) | ((uint64_t)(_val & _mask) << _shift);                         \
        for (uint32_t _i = 0; _i < _n && _i < 5; _i++)                                                                 \
            WRITE8(_ea + _byte_off + _i, (uint8_t)(_data >> ((_n - 1u - _i) * 8u)));                                   \
    } while (0)

// Decode bit-field extension word: returns offset and width; sets CC_N from MSB.
// Uses FETCH16() which advances PC.
#define BF_DECODE_EXT(bf_off, bf_w)                                                                                    \
    do {                                                                                                               \
        uint16_t _ext = FETCH16();                                                                                     \
        (bf_off) = (_ext & 0x0800) ? (int32_t)D((_ext >> 6) & 7) : (int32_t)((_ext >> 6) & 31u);                       \
        (bf_w) = (_ext & 0x0020) ? (D(_ext & 7) & 31u) : (uint32_t)(_ext & 31u);                                       \
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

// --- BFTST ---
#undef OP_BFTST_DN
#define OP_BFTST_DN                                                                                                    \
    OP({                                                                                                               \
        int32_t _off;                                                                                                  \
        uint32_t _w;                                                                                                   \
        BF_DECODE_EXT(_off, _w);                                                                                       \
        uint32_t _f = bf_extract_reg(DY, _off, _w);                                                                    \
        BF_UPDATE_CC(_f, _w);                                                                                          \
    })

#undef OP_BFTST_EA
#define OP_BFTST_EA                                                                                                    \
    OP({                                                                                                               \
        int32_t _off;                                                                                                  \
        uint32_t _w;                                                                                                   \
        BF_DECODE_EXT(_off, _w);                                                                                       \
        uint32_t _ea = CALCULATE_EA(1, EA_MODE, EA_REG, true);                                                         \
        uint32_t _f;                                                                                                   \
        BF_EXTRACT_MEM(_ea, _off, _w, _f);                                                                             \
        BF_UPDATE_CC(_f, _w);                                                                                          \
    })

// --- BFEXTU ---
#undef OP_BFEXTU_DN
#define OP_BFEXTU_DN                                                                                                   \
    OP({                                                                                                               \
        uint16_t _ext = FETCH16();                                                                                     \
        int32_t _off = (_ext & 0x0800) ? (int32_t)D((_ext >> 6) & 7) : (int32_t)((_ext >> 6) & 31u);                   \
        uint32_t _w = (_ext & 0x0020) ? (D(_ext & 7) & 31u) : (uint32_t)(_ext & 31u);                                  \
        if (!_w)                                                                                                       \
            _w = 32u;                                                                                                  \
        uint32_t _dn = (_ext >> 12) & 7u;                                                                              \
        uint32_t _f = bf_extract_reg(DY, _off, _w);                                                                    \
        BF_UPDATE_CC(_f, _w);                                                                                          \
        D(_dn) = _f;                                                                                                   \
    })

#undef OP_BFEXTU_EA
#define OP_BFEXTU_EA                                                                                                   \
    OP({                                                                                                               \
        uint16_t _ext = FETCH16();                                                                                     \
        int32_t _off = (_ext & 0x0800) ? (int32_t)D((_ext >> 6) & 7) : (int32_t)((_ext >> 6) & 31u);                   \
        uint32_t _w = (_ext & 0x0020) ? (D(_ext & 7) & 31u) : (uint32_t)(_ext & 31u);                                  \
        if (!_w)                                                                                                       \
            _w = 32u;                                                                                                  \
        uint32_t _dn = (_ext >> 12) & 7u;                                                                              \
        uint32_t _ea = CALCULATE_EA(1, EA_MODE, EA_REG, true);                                                         \
        uint32_t _f;                                                                                                   \
        BF_EXTRACT_MEM(_ea, _off, _w, _f);                                                                             \
        BF_UPDATE_CC(_f, _w);                                                                                          \
        D(_dn) = _f;                                                                                                   \
    })

// --- BFEXTS ---
#undef OP_BFEXTS_DN
#define OP_BFEXTS_DN                                                                                                   \
    OP({                                                                                                               \
        uint16_t _ext = FETCH16();                                                                                     \
        int32_t _off = (_ext & 0x0800) ? (int32_t)D((_ext >> 6) & 7) : (int32_t)((_ext >> 6) & 31u);                   \
        uint32_t _w = (_ext & 0x0020) ? (D(_ext & 7) & 31u) : (uint32_t)(_ext & 31u);                                  \
        if (!_w)                                                                                                       \
            _w = 32u;                                                                                                  \
        uint32_t _dn = (_ext >> 12) & 7u;                                                                              \
        uint32_t _f = bf_extract_reg(DY, _off, _w);                                                                    \
        BF_UPDATE_CC(_f, _w);                                                                                          \
        /* sign-extend field */                                                                                        \
        D(_dn) = (_w < 32u) ? (uint32_t)((int32_t)(_f << (32u - _w)) >> (32u - _w)) : _f;                              \
    })

#undef OP_BFEXTS_EA
#define OP_BFEXTS_EA                                                                                                   \
    OP({                                                                                                               \
        uint16_t _ext = FETCH16();                                                                                     \
        int32_t _off = (_ext & 0x0800) ? (int32_t)D((_ext >> 6) & 7) : (int32_t)((_ext >> 6) & 31u);                   \
        uint32_t _w = (_ext & 0x0020) ? (D(_ext & 7) & 31u) : (uint32_t)(_ext & 31u);                                  \
        if (!_w)                                                                                                       \
            _w = 32u;                                                                                                  \
        uint32_t _dn = (_ext >> 12) & 7u;                                                                              \
        uint32_t _ea = CALCULATE_EA(1, EA_MODE, EA_REG, true);                                                         \
        uint32_t _f;                                                                                                   \
        BF_EXTRACT_MEM(_ea, _off, _w, _f);                                                                             \
        BF_UPDATE_CC(_f, _w);                                                                                          \
        D(_dn) = (_w < 32u) ? (uint32_t)((int32_t)(_f << (32u - _w)) >> (32u - _w)) : _f;                              \
    })

// --- BFCHG ---
#undef OP_BFCHG_DN
#define OP_BFCHG_DN                                                                                                    \
    OP({                                                                                                               \
        int32_t _off;                                                                                                  \
        uint32_t _w;                                                                                                   \
        BF_DECODE_EXT(_off, _w);                                                                                       \
        uint32_t _f = bf_extract_reg(DY, _off, _w);                                                                    \
        BF_UPDATE_CC(_f, _w);                                                                                          \
        DY = bf_insert_reg(DY, _off, _w, ~_f);                                                                         \
    })

#undef OP_BFCHG_EA
#define OP_BFCHG_EA                                                                                                    \
    OP({                                                                                                               \
        int32_t _off;                                                                                                  \
        uint32_t _w;                                                                                                   \
        BF_DECODE_EXT(_off, _w);                                                                                       \
        uint32_t _ea = CALCULATE_EA(1, EA_MODE, EA_REG, true);                                                         \
        uint32_t _f;                                                                                                   \
        BF_EXTRACT_MEM(_ea, _off, _w, _f);                                                                             \
        BF_UPDATE_CC(_f, _w);                                                                                          \
        BF_INSERT_MEM(_ea, _off, _w, ~_f);                                                                             \
    })

// --- BFCLR ---
#undef OP_BFCLR_DN
#define OP_BFCLR_DN                                                                                                    \
    OP({                                                                                                               \
        int32_t _off;                                                                                                  \
        uint32_t _w;                                                                                                   \
        BF_DECODE_EXT(_off, _w);                                                                                       \
        uint32_t _f = bf_extract_reg(DY, _off, _w);                                                                    \
        BF_UPDATE_CC(_f, _w);                                                                                          \
        DY = bf_insert_reg(DY, _off, _w, 0u);                                                                          \
    })

#undef OP_BFCLR_EA
#define OP_BFCLR_EA                                                                                                    \
    OP({                                                                                                               \
        int32_t _off;                                                                                                  \
        uint32_t _w;                                                                                                   \
        BF_DECODE_EXT(_off, _w);                                                                                       \
        uint32_t _ea = CALCULATE_EA(1, EA_MODE, EA_REG, true);                                                         \
        uint32_t _f;                                                                                                   \
        BF_EXTRACT_MEM(_ea, _off, _w, _f);                                                                             \
        BF_UPDATE_CC(_f, _w);                                                                                          \
        BF_INSERT_MEM(_ea, _off, _w, 0u);                                                                              \
    })

// --- BFSET ---
#undef OP_BFSET_DN
#define OP_BFSET_DN                                                                                                    \
    OP({                                                                                                               \
        int32_t _off;                                                                                                  \
        uint32_t _w;                                                                                                   \
        BF_DECODE_EXT(_off, _w);                                                                                       \
        uint32_t _f = bf_extract_reg(DY, _off, _w);                                                                    \
        BF_UPDATE_CC(_f, _w);                                                                                          \
        DY = bf_insert_reg(DY, _off, _w, 0xFFFFFFFFu);                                                                 \
    })

#undef OP_BFSET_EA
#define OP_BFSET_EA                                                                                                    \
    OP({                                                                                                               \
        int32_t _off;                                                                                                  \
        uint32_t _w;                                                                                                   \
        BF_DECODE_EXT(_off, _w);                                                                                       \
        uint32_t _ea = CALCULATE_EA(1, EA_MODE, EA_REG, true);                                                         \
        uint32_t _f;                                                                                                   \
        BF_EXTRACT_MEM(_ea, _off, _w, _f);                                                                             \
        BF_UPDATE_CC(_f, _w);                                                                                          \
        BF_INSERT_MEM(_ea, _off, _w, 0xFFFFFFFFu);                                                                     \
    })

// --- BFFFO: Bit Field Find First One ---
#undef OP_BFFFO_DN
#define OP_BFFFO_DN                                                                                                    \
    OP({                                                                                                               \
        uint16_t _ext = FETCH16();                                                                                     \
        int32_t _off = (_ext & 0x0800) ? (int32_t)D((_ext >> 6) & 7) : (int32_t)((_ext >> 6) & 31u);                   \
        uint32_t _w = (_ext & 0x0020) ? (D(_ext & 7) & 31u) : (uint32_t)(_ext & 31u);                                  \
        if (!_w)                                                                                                       \
            _w = 32u;                                                                                                  \
        uint32_t _dn = (_ext >> 12) & 7u;                                                                              \
        uint32_t _f = bf_extract_reg(DY, _off, _w);                                                                    \
        BF_UPDATE_CC(_f, _w);                                                                                          \
        uint32_t _pos = 0;                                                                                             \
        if (_f != 0) {                                                                                                 \
            for (_pos = 0; _pos < _w && !((_f >> (_w - 1u - _pos)) & 1u); _pos++) {                                    \
            }                                                                                                          \
        } else {                                                                                                       \
            _pos = _w;                                                                                                 \
        }                                                                                                              \
        D(_dn) = (uint32_t)(_off & 31) + _pos;                                                                         \
    })

#undef OP_BFFFO_EA
#define OP_BFFFO_EA                                                                                                    \
    OP({                                                                                                               \
        uint16_t _ext = FETCH16();                                                                                     \
        int32_t _off = (_ext & 0x0800) ? (int32_t)D((_ext >> 6) & 7) : (int32_t)((_ext >> 6) & 31u);                   \
        uint32_t _w = (_ext & 0x0020) ? (D(_ext & 7) & 31u) : (uint32_t)(_ext & 31u);                                  \
        if (!_w)                                                                                                       \
            _w = 32u;                                                                                                  \
        uint32_t _dn = (_ext >> 12) & 7u;                                                                              \
        uint32_t _ea = CALCULATE_EA(1, EA_MODE, EA_REG, true);                                                         \
        uint32_t _f;                                                                                                   \
        BF_EXTRACT_MEM(_ea, _off, _w, _f);                                                                             \
        BF_UPDATE_CC(_f, _w);                                                                                          \
        uint32_t _pos = 0;                                                                                             \
        if (_f != 0) {                                                                                                 \
            for (_pos = 0; _pos < _w && !((_f >> (_w - 1u - _pos)) & 1u); _pos++) {                                    \
            }                                                                                                          \
        } else {                                                                                                       \
            _pos = _w;                                                                                                 \
        }                                                                                                              \
        D(_dn) = (uint32_t)_off + _pos;                                                                                \
    })

// --- BFINS: Bit Field Insert ---
#undef OP_BFINS_DN
#define OP_BFINS_DN                                                                                                    \
    OP({                                                                                                               \
        uint16_t _ext = FETCH16();                                                                                     \
        int32_t _off = (_ext & 0x0800) ? (int32_t)D((_ext >> 6) & 7) : (int32_t)((_ext >> 6) & 31u);                   \
        uint32_t _w = (_ext & 0x0020) ? (D(_ext & 7) & 31u) : (uint32_t)(_ext & 31u);                                  \
        if (!_w)                                                                                                       \
            _w = 32u;                                                                                                  \
        uint32_t _dn = (_ext >> 12) & 7u;                                                                              \
        uint32_t _src = D(_dn);                                                                                        \
        uint32_t _mask = (_w == 32u) ? 0xFFFFFFFFu : ((1u << _w) - 1u);                                                \
        uint32_t _f = _src & _mask;                                                                                    \
        BF_UPDATE_CC(_f, _w);                                                                                          \
        DY = bf_insert_reg(DY, _off, _w, _f);                                                                          \
    })

#undef OP_BFINS_EA
#define OP_BFINS_EA                                                                                                    \
    OP({                                                                                                               \
        uint16_t _ext = FETCH16();                                                                                     \
        int32_t _off = (_ext & 0x0800) ? (int32_t)D((_ext >> 6) & 7) : (int32_t)((_ext >> 6) & 31u);                   \
        uint32_t _w = (_ext & 0x0020) ? (D(_ext & 7) & 31u) : (uint32_t)(_ext & 31u);                                  \
        if (!_w)                                                                                                       \
            _w = 32u;                                                                                                  \
        uint32_t _dn = (_ext >> 12) & 7u;                                                                              \
        uint32_t _mask = (_w == 32u) ? 0xFFFFFFFFu : ((1u << _w) - 1u);                                                \
        uint32_t _f = D(_dn) & _mask;                                                                                  \
        BF_UPDATE_CC(_f, _w);                                                                                          \
        uint32_t _ea = CALCULATE_EA(1, EA_MODE, EA_REG, true);                                                         \
        BF_INSERT_MEM(_ea, _off, _w, _f);                                                                              \
    })

// --- LINK.L (32-bit displacement) ---
#undef OP_LINK_L_AN_DISP
#define OP_LINK_L_AN_DISP                                                                                              \
    OP({                                                                                                               \
        uint32_t _a = AY;                                                                                              \
        PUSH32(_a);                                                                                                    \
        AY = SP;                                                                                                       \
        SP += (int32_t)FETCH32();                                                                                      \
    })

// --- RTD: Return and Deallocate ---
#undef OP_RTD_DISPLACEMENT
#define OP_RTD_DISPLACEMENT                                                                                            \
    OP({                                                                                                               \
        POP32(PC);                                                                                                     \
        int16_t _d = (int16_t)FETCH16();                                                                               \
        SP += (int32_t)_d;                                                                                             \
    })

// --- TRAPcc: Trap on Condition ---
#undef OP_TRAPCC
#define OP_TRAPCC OP(if (CC) EXC_TRAPV())

#undef OP_TRAPCC_W_DATA
#define OP_TRAPCC_W_DATA                                                                                               \
    OP({                                                                                                               \
        (void)FETCH16();                                                                                               \
        if (CC)                                                                                                        \
            EXC_TRAPV();                                                                                               \
    })

#undef OP_TRAPCC_L_DATA
#define OP_TRAPCC_L_DATA                                                                                               \
    OP({                                                                                                               \
        (void)FETCH32();                                                                                               \
        if (CC)                                                                                                        \
            EXC_TRAPV();                                                                                               \
    })

// --- CHK.L <ea>,Dn (32-bit bounds check) ---
#undef OP_CHK_L_EA_DN
#define OP_CHK_L_EA_DN                                                                                                 \
    OP(                                                                                                                \
        VALID_EA(ea_data); LOAD_EA_WITH_UPDATE(32, _bound); int32_t _dn = (int32_t)DX; int32_t _b = (int32_t)_bound;   \
        if (_dn < 0) {                                                                                                 \
            CC_N = 1;                                                                                                  \
            EXC_CHK();                                                                                                 \
        } else if (_dn > _b) {                                                                                         \
            CC_N = 0;                                                                                                  \
            EXC_CHK();                                                                                                 \
        })

// --- MOVEC: Move Control Register ---
// MOVEC Rc,Rn: privileged, reads control register into general register
#undef OP_MOVEC_RC_RN
#define OP_MOVEC_RC_RN                                                                                                 \
    OP(SUPER({                                                                                                         \
        uint16_t _ext = FETCH16();                                                                                     \
        uint32_t _da = (_ext >> 15) & 1u;                                                                              \
        uint32_t _rn = (_ext >> 12) & 7u;                                                                              \
        uint16_t _rc = _ext & 0x0FFFu;                                                                                 \
        uint32_t _val = 0;                                                                                             \
        switch (_rc) {                                                                                                 \
        case 0x000:                                                                                                    \
            _val = cpu->sfc;                                                                                           \
            break;                                                                                                     \
        case 0x001:                                                                                                    \
            _val = cpu->dfc;                                                                                           \
            break;                                                                                                     \
        case 0x002:                                                                                                    \
            _val = cpu->cacr;                                                                                          \
            break;                                                                                                     \
        case 0x800:                                                                                                    \
            _val = IS_SUPERVISOR() ? cpu->usp : GET_USP();                                                             \
            break;                                                                                                     \
        case 0x801:                                                                                                    \
            _val = cpu->vbr;                                                                                           \
            break;                                                                                                     \
        case 0x802:                                                                                                    \
            _val = cpu->caar;                                                                                          \
            break;                                                                                                     \
        case 0x803:                                                                                                    \
            _val = cpu->m ? A(7) : cpu->msp;                                                                           \
            break;                                                                                                     \
        case 0x804:                                                                                                    \
            _val = cpu->m ? cpu->ssp : A(7);                                                                           \
            break;                                                                                                     \
        default:                                                                                                       \
            EXC_ILLEGAL();                                                                                             \
            continue;                                                                                                  \
        }                                                                                                              \
        if (_da)                                                                                                       \
            A(_rn) = _val;                                                                                             \
        else                                                                                                           \
            D(_rn) = _val;                                                                                             \
    }))

// MOVEC Rn,Rc: privileged, writes general register to control register
#undef OP_MOVEC_RN_RC
#define OP_MOVEC_RN_RC                                                                                                 \
    OP(SUPER({                                                                                                         \
        uint16_t _ext = FETCH16();                                                                                     \
        uint32_t _da = (_ext >> 15) & 1u;                                                                              \
        uint32_t _rn = (_ext >> 12) & 7u;                                                                              \
        uint16_t _rc = _ext & 0x0FFFu;                                                                                 \
        uint32_t _val = _da ? A(_rn) : D(_rn);                                                                         \
        switch (_rc) {                                                                                                 \
        case 0x000:                                                                                                    \
            cpu->sfc = _val & 7u;                                                                                      \
            break;                                                                                                     \
        case 0x001:                                                                                                    \
            cpu->dfc = _val & 7u;                                                                                      \
            break;                                                                                                     \
        case 0x002:                                                                                                    \
            cpu->cacr = _val;                                                                                          \
            break; /* cache ops: no-op */                                                                              \
        case 0x800:                                                                                                    \
            SET_USP(_val);                                                                                             \
            break;                                                                                                     \
        case 0x801:                                                                                                    \
            cpu->vbr = _val;                                                                                           \
            break;                                                                                                     \
        case 0x802:                                                                                                    \
            cpu->caar = _val;                                                                                          \
            break; /* cache addr: no-op */                                                                             \
        case 0x803:                                                                                                    \
            if (cpu->m)                                                                                                \
                A(7) = _val;                                                                                           \
            else                                                                                                       \
                cpu->msp = _val;                                                                                       \
            break;                                                                                                     \
        case 0x804:                                                                                                    \
            if (!cpu->m)                                                                                               \
                A(7) = _val;                                                                                           \
            else                                                                                                       \
                cpu->ssp = _val;                                                                                       \
            break;                                                                                                     \
        default:                                                                                                       \
            EXC_ILLEGAL();                                                                                             \
            continue;                                                                                                  \
        }                                                                                                              \
    }))

// --- MULS.L / DIVS.L: 32x32->64 multiply, 64/32 divide ---
// MULS.L <ea>,Dh:Dl  (or MULU.L)
// Extension word: 0000 Dh 1S 00 Dl  (S=1 for signed)
// When Dh==Dl: only 32-bit result in Dl (Dh ignored per spec)
#undef OP_MULS_L_EA_DH_DL
#define OP_MULS_L_EA_DH_DL                                                                                             \
    OP({                                                                                                               \
        uint16_t _ext = FETCH16();                                                                                     \
        uint32_t _dh = (_ext >> 12) & 7u;                                                                              \
        uint32_t _dl = _ext & 7u;                                                                                      \
        int _signed = (_ext >> 11) & 1;                                                                                \
        VALID_EA(ea_data);                                                                                             \
        LOAD_EA_WITH_UPDATE(32, _src);                                                                                 \
        if (_signed) {                                                                                                 \
            int64_t _res = (int64_t)(int32_t)D(_dl) * (int64_t)(int32_t)_src;                                          \
            D(_dl) = (uint32_t)_res;                                                                                   \
            if (_dh != _dl)                                                                                            \
                D(_dh) = (uint32_t)((uint64_t)_res >> 32);                                                             \
            CC_N = (_res < 0);                                                                                         \
            CC_Z = (_res == 0);                                                                                        \
            CC_V = CC_C = 0;                                                                                           \
        } else {                                                                                                       \
            uint64_t _res = (uint64_t)D(_dl) * (uint64_t)_src;                                                         \
            D(_dl) = (uint32_t)_res;                                                                                   \
            if (_dh != _dl)                                                                                            \
                D(_dh) = (uint32_t)(_res >> 32);                                                                       \
            CC_N = (_res >> 63);                                                                                       \
            CC_Z = (_res == 0);                                                                                        \
            CC_V = CC_C = 0;                                                                                           \
        }                                                                                                              \
    })

// DIVS.L/DIVU.L <ea>,Dr:Dq  (64/32 or 32/32 divide)
// Extension word: 0000 Dr 1S 00 Dq  (S=1 signed)
// When Dr==Dq: 32/32 divide (quotient in Dq, no remainder stored)
#undef OP_DIVS_L_EA_DR_DQ
#define OP_DIVS_L_EA_DR_DQ                                                                                             \
    OP({                                                                                                               \
        uint16_t _ext = FETCH16();                                                                                     \
        uint32_t _dr = (_ext >> 12) & 7u;                                                                              \
        uint32_t _dq = _ext & 7u;                                                                                      \
        int _signed = (_ext >> 11) & 1;                                                                                \
        VALID_EA(ea_data);                                                                                             \
        LOAD_EA_WITH_UPDATE(32, _divisor);                                                                             \
        CLEAR_NZVC();                                                                                                  \
        if (!_divisor) {                                                                                               \
            EXC_DIVIDE_BY_ZERO();                                                                                      \
        } else {                                                                                                       \
            if (_signed) {                                                                                             \
                int64_t _dividend =                                                                                    \
                    (_dr == _dq) ? (int64_t)(int32_t)D(_dq) : (int64_t)(((uint64_t)D(_dr) << 32) | D(_dq));            \
                int64_t _q = _dividend / (int32_t)_divisor;                                                            \
                int64_t _r = _dividend % (int32_t)_divisor;                                                            \
                if (_q > INT32_MAX || _q < INT32_MIN) {                                                                \
                    CC_V = CC_N = 1;                                                                                   \
                } else {                                                                                               \
                    D(_dq) = (uint32_t)_q;                                                                             \
                    if (_dr != _dq)                                                                                    \
                        D(_dr) = (uint32_t)_r;                                                                         \
                    CC_N = (_q < 0);                                                                                   \
                    CC_Z = (_q == 0);                                                                                  \
                }                                                                                                      \
            } else {                                                                                                   \
                uint64_t _dividend = (_dr == _dq) ? (uint64_t)D(_dq) : (((uint64_t)D(_dr) << 32) | D(_dq));            \
                uint64_t _q = _dividend / (uint32_t)_divisor;                                                          \
                uint64_t _r = _dividend % (uint32_t)_divisor;                                                          \
                if (_q > UINT32_MAX) {                                                                                 \
                    CC_V = CC_N = 1;                                                                                   \
                } else {                                                                                               \
                    D(_dq) = (uint32_t)_q;                                                                             \
                    if (_dr != _dq)                                                                                    \
                        D(_dr) = (uint32_t)_r;                                                                         \
                    CC_N = (uint32_t)_q >> 31;                                                                         \
                    CC_Z = (_q == 0);                                                                                  \
                }                                                                                                      \
            }                                                                                                          \
        }                                                                                                              \
    })

// --- CAS: Compare And Swap (stub — atomic operations not needed for single-CPU) ---
#undef OP_CAS_B_DC_DU_EA
#define OP_CAS_B_DC_DU_EA                                                                                              \
    OP({                                                                                                               \
        uint16_t _ext = FETCH16();                                                                                     \
        uint32_t _du = (_ext >> 6) & 7u;                                                                               \
        uint32_t _dc = _ext & 7u;                                                                                      \
        VALID_EA(ea_memory &ea_alterable);                                                                             \
        uint8_t _mem = (uint8_t)READ_EA(8, opcode, false);                                                             \
        uint8_t _cmp = (uint8_t)D(_dc);                                                                                \
        uint8_t _res;                                                                                                  \
        GENERIC_SUB(_cmp, _mem, _res);                                                                                 \
        if (CC_Z) {                                                                                                    \
            STORE_EA(8, (uint8_t)D(_du));                                                                              \
        } else {                                                                                                       \
            STORE_DN(8, _dc, _mem);                                                                                    \
        }                                                                                                              \
    })

#undef OP_CAS_W_DC_DU_EA
#define OP_CAS_W_DC_DU_EA                                                                                              \
    OP({                                                                                                               \
        uint16_t _ext = FETCH16();                                                                                     \
        uint32_t _du = (_ext >> 6) & 7u;                                                                               \
        uint32_t _dc = _ext & 7u;                                                                                      \
        VALID_EA(ea_memory &ea_alterable);                                                                             \
        uint16_t _mem = (uint16_t)READ_EA(16, opcode, false);                                                          \
        uint16_t _cmp = (uint16_t)D(_dc);                                                                              \
        uint16_t _res;                                                                                                 \
        GENERIC_SUB(_cmp, _mem, _res);                                                                                 \
        if (CC_Z) {                                                                                                    \
            STORE_EA(16, (uint16_t)D(_du));                                                                            \
        } else {                                                                                                       \
            STORE_DN(16, _dc, _mem);                                                                                   \
        }                                                                                                              \
    })

#undef OP_CAS_L_DC_DU_EA
#define OP_CAS_L_DC_DU_EA                                                                                              \
    OP({                                                                                                               \
        uint16_t _ext = FETCH16();                                                                                     \
        uint32_t _du = (_ext >> 6) & 7u;                                                                               \
        uint32_t _dc = _ext & 7u;                                                                                      \
        VALID_EA(ea_memory &ea_alterable);                                                                             \
        uint32_t _mem = (uint32_t)READ_EA(32, opcode, false);                                                          \
        uint32_t _cmp = D(_dc);                                                                                        \
        uint32_t _res;                                                                                                 \
        GENERIC_SUB(_cmp, _mem, _res);                                                                                 \
        if (CC_Z) {                                                                                                    \
            STORE_EA(32, D(_du));                                                                                      \
        } else {                                                                                                       \
            D(_dc) = _mem;                                                                                             \
        }                                                                                                              \
    })

// CAS2: two-operand compare-and-swap (stub — single-CPU, no actual atomicity needed)
#undef OP_CAS2_W_DC_DU_RN
#define OP_CAS2_W_DC_DU_RN                                                                                             \
    OP({                                                                                                               \
        uint16_t _e1 = FETCH16();                                                                                      \
        uint16_t _e2 = FETCH16();                                                                                      \
        uint32_t _rn1 = (_e1 & 0x8000) ? A((_e1 >> 12) & 7) : D((_e1 >> 12) & 7);                                      \
        uint32_t _rn2 = (_e2 & 0x8000) ? A((_e2 >> 12) & 7) : D((_e2 >> 12) & 7);                                      \
        uint32_t _dc1 = _e1 & 7u;                                                                                      \
        uint32_t _du1 = (_e1 >> 6) & 7u;                                                                               \
        uint32_t _dc2 = _e2 & 7u;                                                                                      \
        uint32_t _du2 = (_e2 >> 6) & 7u;                                                                               \
        uint16_t _m1 = READ16(_rn1);                                                                                   \
        uint16_t _m2 = READ16(_rn2);                                                                                   \
        uint16_t _r1;                                                                                                  \
        GENERIC_SUB((uint16_t)D(_dc1), _m1, _r1);                                                                      \
        if (CC_Z) {                                                                                                    \
            uint16_t _r2;                                                                                              \
            GENERIC_SUB((uint16_t)D(_dc2), _m2, _r2);                                                                  \
            if (CC_Z) {                                                                                                \
                WRITE16(_rn1, (uint16_t)D(_du1));                                                                      \
                WRITE16(_rn2, (uint16_t)D(_du2));                                                                      \
            } else {                                                                                                   \
                STORE_DN(16, _dc1, _m1);                                                                               \
                STORE_DN(16, _dc2, _m2);                                                                               \
            }                                                                                                          \
        } else {                                                                                                       \
            STORE_DN(16, _dc1, _m1);                                                                                   \
            STORE_DN(16, _dc2, _m2);                                                                                   \
        }                                                                                                              \
    })

#undef OP_CAS2_L_DC_DU_RN
#define OP_CAS2_L_DC_DU_RN                                                                                             \
    OP({                                                                                                               \
        uint16_t _e1 = FETCH16();                                                                                      \
        uint16_t _e2 = FETCH16();                                                                                      \
        uint32_t _rn1 = (_e1 & 0x8000) ? A((_e1 >> 12) & 7) : D((_e1 >> 12) & 7);                                      \
        uint32_t _rn2 = (_e2 & 0x8000) ? A((_e2 >> 12) & 7) : D((_e2 >> 12) & 7);                                      \
        uint32_t _dc1 = _e1 & 7u;                                                                                      \
        uint32_t _du1 = (_e1 >> 6) & 7u;                                                                               \
        uint32_t _dc2 = _e2 & 7u;                                                                                      \
        uint32_t _du2 = (_e2 >> 6) & 7u;                                                                               \
        uint32_t _m1 = READ32(_rn1);                                                                                   \
        uint32_t _m2 = READ32(_rn2);                                                                                   \
        uint32_t _r1;                                                                                                  \
        GENERIC_SUB(D(_dc1), _m1, _r1);                                                                                \
        if (CC_Z) {                                                                                                    \
            uint32_t _r2;                                                                                              \
            GENERIC_SUB(D(_dc2), _m2, _r2);                                                                            \
            if (CC_Z) {                                                                                                \
                WRITE32(_rn1, D(_du1));                                                                                \
                WRITE32(_rn2, D(_du2));                                                                                \
            } else {                                                                                                   \
                D(_dc1) = _m1;                                                                                         \
                D(_dc2) = _m2;                                                                                         \
            }                                                                                                          \
        } else {                                                                                                       \
            D(_dc1) = _m1;                                                                                             \
            D(_dc2) = _m2;                                                                                             \
        }                                                                                                              \
    })

// --- CHK2/CMP2: Compare with bounds ---
// Extension word: Dn/An:Rn at bits 15:12, IS bit 11 (0=CMP2, 1=CHK2)
// Compares Rn against lower bound at EA and upper bound at EA+size.
#define CHK2_CMP2(bits)                                                                                                \
    {                                                                                                                  \
        uint16_t _ext = FETCH16();                                                                                     \
        uint32_t _da = (_ext >> 15) & 1u;                                                                              \
        uint32_t _rn = (_ext >> 12) & 7u;                                                                              \
        int _is_chk2 = (_ext >> 11) & 1;                                                                               \
        VALID_EA(ea_control);                                                                                          \
        uint32_t _ea = GET_EA;                                                                                         \
        uint32_t _lo, _hi, _val;                                                                                       \
        if (bits == 8) {                                                                                               \
            _lo = (uint32_t)(int32_t)(int8_t)READ8(_ea);                                                               \
            _hi = (uint32_t)(int32_t)(int8_t)READ8(_ea + 1);                                                           \
            _val = _da ? A(_rn) : (uint32_t)(int32_t)(int8_t)D(_rn);                                                   \
        } else if (bits == 16) {                                                                                       \
            _lo = (uint32_t)(int32_t)(int16_t)READ16(_ea);                                                             \
            _hi = (uint32_t)(int32_t)(int16_t)READ16(_ea + 2);                                                         \
            _val = _da ? A(_rn) : (uint32_t)(int32_t)(int16_t)D(_rn);                                                  \
        } else {                                                                                                       \
            _lo = READ32(_ea);                                                                                         \
            _hi = READ32(_ea + 4);                                                                                     \
            _val = _da ? A(_rn) : D(_rn);                                                                              \
        }                                                                                                              \
        CC_Z = (_val == _lo || _val == _hi);                                                                           \
        CC_C = ((bits == 32) ? (_val < _lo || _val > _hi)                                                              \
                             : ((int32_t)_val < (int32_t)_lo || (int32_t)_val > (int32_t)_hi));                        \
        if (_is_chk2 && CC_C)                                                                                          \
            EXC_CHK();                                                                                                 \
    }

#undef OP_CHK2_B_EA_DN
#define OP_CHK2_B_EA_DN OP(CHK2_CMP2(8))
#undef OP_CHK2_W_EA_DN
#define OP_CHK2_W_EA_DN OP(CHK2_CMP2(16))
#undef OP_CHK2_L_EA_DN
#define OP_CHK2_L_EA_DN OP(CHK2_CMP2(32))

// --- PACK/UNPK: BCD pack/unpack ---
// PACK DY,DX,#adj: ((DY + adj) & 0xFF) → low byte nibbles → DX
#undef OP_PACK_DY_DX
#define OP_PACK_DY_DX                                                                                                  \
    OP({                                                                                                               \
        uint16_t _adj = FETCH16();                                                                                     \
        uint16_t _src = (uint16_t)(DY + _adj);                                                                         \
        STORE_DN(8, opcode >> 9 & 7, ((_src >> 4) & 0xF0) | ((_src) & 0x0F));                                          \
    })

// PACK -(AY),-(AX),#adj: from memory
#undef OP_PACK_AY_AX
#define OP_PACK_AY_AX                                                                                                  \
    OP({                                                                                                               \
        uint16_t _adj = FETCH16();                                                                                     \
        A(EA_REG) -= 2;                                                                                                \
        uint16_t _src = READ16(A(EA_REG));                                                                             \
        _src = (uint16_t)(_src + _adj);                                                                                \
        A(opcode >> 9 & 7) -= 1;                                                                                       \
        WRITE8(A(opcode >> 9 & 7), (uint8_t)(((_src >> 4) & 0xF0) | ((_src) & 0x0F)));                                 \
    })

// UNPK DY,DX,#adj: separate two BCD nibbles, add adj
#undef OP_UNPK_DY_DX
#define OP_UNPK_DY_DX                                                                                                  \
    OP({                                                                                                               \
        uint16_t _adj = FETCH16();                                                                                     \
        uint8_t _b = (uint8_t)DY;                                                                                      \
        uint16_t _res = (uint16_t)((((_b >> 4) & 0xF) << 8) | ((_b) & 0xF)) + _adj;                                    \
        STORE_DN(16, opcode >> 9 & 7, _res);                                                                           \
    })

// UNPK -(AY),-(AX),#adj: from/to memory
#undef OP_UNPK_AY_AX
#define OP_UNPK_AY_AX                                                                                                  \
    OP({                                                                                                               \
        uint16_t _adj = FETCH16();                                                                                     \
        A(EA_REG) -= 1;                                                                                                \
        uint8_t _b = READ8(A(EA_REG));                                                                                 \
        uint16_t _res = (uint16_t)((((_b >> 4) & 0xF) << 8) | ((_b) & 0xF)) + _adj;                                    \
        A(opcode >> 9 & 7) -= 2;                                                                                       \
        WRITE16(A(opcode >> 9 & 7), _res);                                                                             \
    })

// --- MOVES: Move with Alternate Function Code (supervisor, stub) ---
// MOVES reads/writes with SFC/DFC; since we don't have MMU translation,
// treat as a normal privileged memory access.
#undef OP_MOVES_B_RN_EA
#define OP_MOVES_B_RN_EA                                                                                               \
    OP(SUPER({                                                                                                         \
        uint16_t _ext = FETCH16();                                                                                     \
        uint32_t _da = (_ext >> 15) & 1u;                                                                              \
        uint32_t _rn = (_ext >> 12) & 7u;                                                                              \
        VALID_EA(ea_memory &ea_alterable);                                                                             \
        uint32_t _ea = CALCULATE_EA(1, EA_MODE, EA_REG, true);                                                         \
        WRITE8(_ea, (uint8_t)(_da ? A(_rn) : D(_rn)));                                                                 \
    }))

#undef OP_MOVES_W_RN_EA
#define OP_MOVES_W_RN_EA                                                                                               \
    OP(SUPER({                                                                                                         \
        uint16_t _ext = FETCH16();                                                                                     \
        uint32_t _da = (_ext >> 15) & 1u;                                                                              \
        uint32_t _rn = (_ext >> 12) & 7u;                                                                              \
        VALID_EA(ea_memory &ea_alterable);                                                                             \
        uint32_t _ea = CALCULATE_EA(2, EA_MODE, EA_REG, true);                                                         \
        WRITE16(_ea, (uint16_t)(_da ? A(_rn) : D(_rn)));                                                               \
    }))

#undef OP_MOVES_L_RN_EA
#define OP_MOVES_L_RN_EA                                                                                               \
    OP(SUPER({                                                                                                         \
        uint16_t _ext = FETCH16();                                                                                     \
        uint32_t _da = (_ext >> 15) & 1u;                                                                              \
        uint32_t _rn = (_ext >> 12) & 7u;                                                                              \
        VALID_EA(ea_memory &ea_alterable);                                                                             \
        uint32_t _ea = CALCULATE_EA(4, EA_MODE, EA_REG, true);                                                         \
        WRITE32(_ea, _da ? A(_rn) : D(_rn));                                                                           \
    }))

#undef OP_MOVES_B_EA_RN
#define OP_MOVES_B_EA_RN                                                                                               \
    OP(SUPER({                                                                                                         \
        uint16_t _ext = FETCH16();                                                                                     \
        uint32_t _da = (_ext >> 15) & 1u;                                                                              \
        uint32_t _rn = (_ext >> 12) & 7u;                                                                              \
        VALID_EA(ea_memory);                                                                                           \
        uint32_t _ea = CALCULATE_EA(1, EA_MODE, EA_REG, true);                                                         \
        if (_da)                                                                                                       \
            A(_rn) = (int32_t)(int8_t)READ8(_ea);                                                                      \
        else                                                                                                           \
            STORE_DN(8, _rn, READ8(_ea));                                                                              \
    }))

#undef OP_MOVES_W_EA_RN
#define OP_MOVES_W_EA_RN                                                                                               \
    OP(SUPER({                                                                                                         \
        uint16_t _ext = FETCH16();                                                                                     \
        uint32_t _da = (_ext >> 15) & 1u;                                                                              \
        uint32_t _rn = (_ext >> 12) & 7u;                                                                              \
        VALID_EA(ea_memory);                                                                                           \
        uint32_t _ea = CALCULATE_EA(2, EA_MODE, EA_REG, true);                                                         \
        if (_da)                                                                                                       \
            A(_rn) = (int32_t)(int16_t)READ16(_ea);                                                                    \
        else                                                                                                           \
            STORE_DN(16, _rn, READ16(_ea));                                                                            \
    }))

#undef OP_MOVES_L_EA_RN
#define OP_MOVES_L_EA_RN                                                                                               \
    OP(SUPER({                                                                                                         \
        uint16_t _ext = FETCH16();                                                                                     \
        uint32_t _da = (_ext >> 15) & 1u;                                                                              \
        uint32_t _rn = (_ext >> 12) & 7u;                                                                              \
        VALID_EA(ea_memory);                                                                                           \
        uint32_t _ea = CALCULATE_EA(4, EA_MODE, EA_REG, true);                                                         \
        if (_da)                                                                                                       \
            A(_rn) = READ32(_ea);                                                                                      \
        else                                                                                                           \
            D(_rn) = READ32(_ea);                                                                                      \
    }))

// --- BKPT: Software Breakpoint (generate BKPT trap = vector 4 illegal instruction) ---
#undef OP_BKPT_DATA
#define OP_BKPT_DATA OP(EXC_ILLEGAL())

// --- MOVE CCR,<ea>: read CCR byte into EA (not privileged) ---
#undef OP_MOVE_B_CCR_EA
#define OP_MOVE_B_CCR_EA OP(VALID_EA(ea_data &ea_alterable); STORE_EA(8, READ_CCR()))

// --- MOVE SR,<ea>: privileged on 68030 (not on 68000) ---
#undef OP_MOVE_W_SR_EA
#define OP_MOVE_W_SR_EA OP(VALID_EA(ea_data &ea_alterable); SUPER(STORE_EA(16, GET_SR())))

// --- RTE: Return from Exception (pop format/vector word on 68030) ---
#undef OP_RTE
#define OP_RTE                                                                                                         \
    OP(SUPER({                                                                                                         \
        uint16_t _sr;                                                                                                  \
        POP16(_sr);                                                                                                    \
        POP32(PC);                                                                                                     \
        uint16_t _fmt;                                                                                                 \
        POP16(_fmt);                                                                                                   \
        SET_SR(_sr);                                                                                                   \
        /* format $0: no additional words; other formats would need special handling */                                \
    }))

// --- MMU branch conditionals: stub as not-taken (MMU conditions always false) ---
#undef OP_PBCC_W
#define OP_PBCC_W OP(PC += 2) // skip branch displacement, never branch

#undef OP_PBCC_L
#define OP_PBCC_L OP(PC += 4) // skip long branch displacement, never branch

// --- MMU PSAVE/PRESTORE: privileged stubs (consume EA extension words) ---
#undef OP_PSAVE_EA
#define OP_PSAVE_EA                                                                                                    \
    OP(SUPER({                                                                                                         \
        VALID_EA(ea_control &ea_alterable);                                                                            \
        (void)GET_EA;                                                                                                  \
    }))

#undef OP_PRESTORE_EA
#define OP_PRESTORE_EA                                                                                                 \
    OP(SUPER({                                                                                                         \
        VALID_EA(ea_control);                                                                                          \
        (void)GET_EA;                                                                                                  \
    }))

// --- FTRAP default: CpID=0 → 68030 MMU stub, else F-line exception ---
#undef OP_FTRAP
#define OP_FTRAP                                                                                                       \
    OP({                                                                                                               \
        if (((opcode >> 9) & 7u) == 0u) {                                                                              \
            /* CpID=0: 68030 MMU instruction (PMOVE/PFLUSH/PTEST) — consume ext word */                              \
            (void)FETCH16();                                                                                           \
        } else {                                                                                                       \
            EXC_FTRAP();                                                                                               \
        }                                                                                                              \
    })

// --- EXTB.L: override LEA to handle EXTB.L Dn (mode=0 EA) ---
// EXTB.L Dn is encoded as opcode 0x49C0+Dn, which falls in the LEA A4 case.
// When EA mode == 0 (register Dn), treat as EXTB.L; otherwise treat as LEA.
#undef OP_LEA_EA_AN
#define OP_LEA_EA_AN                                                                                                   \
    OP(                                                                                                                \
        if (EA_MODE == 0) {                                                                                            \
            uint32_t _r = (uint32_t)(int32_t)(int8_t)DY;                                                               \
            DY = _r;                                                                                                   \
            UPDATE_NZ_CLEAR_CV(_r);                                                                                    \
        } else {                                                                                                       \
            VALID_EA(ea_control);                                                                                      \
            AX = GET_EA;                                                                                               \
        })

#endif // CPU_DECODER_IS_68030

#endif // CPU_OPS_H
