// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_decode.h
// Instruction decoder for the PPC (MPC601) core.
// Note: this header is a template intended for multiple inclusion with
// different macro parameters; it intentionally has no include guard (the
// cpu_decode.h pattern — proposal-heterogeneous-multi-cpu.md §3.3.1).
// Current includers: ppc_run.c (execution) and ppc_disasm.c (printing).
// Because emulator and disassembler are literally the same decode tree,
// they cannot drift out of sync.
//
// The tree decides VALIDITY as well as identity: invalid forms (reserved
// fields set, invalid BO encodings, instructions the 601 lacks — 601UM
// §10.3 Tables 10-6/10-8) route to OP_ILLEGAL on both sides.  Leaves name
// what was recognized and nothing more; Rc/OE/field variants are read from
// `iw` by the includer's OP_ overloads.

// Required macro configuration (provided by includer):
// - PPC_DECODER_NAME:        Symbol/name of the generated decoder function
// - PPC_DECODER_RETURN_TYPE: Return type of the decoder function
// - PPC_DECODER_ARGS:        Formal parameter list (must bind `iw`)
// - PPC_DECODER_PROLOGUE:    Code emitted at function start
// - PPC_DECODER_EPILOGUE:    Code emitted before returning
// - OP_*:                    One leaf macro per instruction (object-like)
// - PPC_OPCD/PPC_RT/... :    The field-extraction macros (the emulator gets
//                            them from ppc_internal.h; the dependency-free
//                            disassembler defines its own matching copy)

#ifndef PPC_DECODER_NAME
#error "Decoder name macro not defined"
#endif
#if !defined(PPC_DECODER_RETURN_TYPE)
#error "Decoder return type macro not defined"
#endif
#if !defined(PPC_DECODER_ARGS)
#error "Decoder args macro not defined"
#endif
#if !defined(PPC_DECODER_PROLOGUE)
#error "Decoder prologue macro not defined"
#endif
#if !defined(PPC_DECODER_EPILOGUE)
#error "Decoder epilogue macro not defined"
#endif

// Valid BO encodings: the z bits of the 1z00y/1z01y/1z1zz groups must be
// zero (invalid forms otherwise); the low bits of the condition-only
// groups are prediction hints and always decode.
#ifndef PPC_BO_VALID_DEFINED
#define PPC_BO_VALID_DEFINED
static inline int ppc_bo_valid(uint32_t bo) {
    if (bo & 0x10u) // 1z00y / 1z01y / 1z1zz groups
        return (bo & 0x04u) ? bo == 0x14u : (bo & 0x08u) == 0;
    return 1; // 0000y/0001y/001at/0100y/0101y/011at
}
#endif // PPC_BO_VALID_DEFINED

// clang-format off

PPC_DECODER_RETURN_TYPE PPC_DECODER_NAME(PPC_DECODER_ARGS) {

    PPC_DECODER_PROLOGUE;

    switch (PPC_OPCD(iw)) {

    case 3:  OP_TWI; break;
    case 7:  OP_MULLI; break;
    case 8:  OP_SUBFIC; break;
    case 9:  OP_DOZI; break;                                   // POWER
    case 10: OP_CMPLI; break;
    case 11: OP_CMPI; break;
    case 12: OP_ADDIC; break;
    case 13: OP_ADDIC_DOT; break;
    case 14: OP_ADDI; break;
    case 15: OP_ADDIS; break;
    case 16: if (!ppc_bo_valid(PPC_RT(iw))) { OP_ILLEGAL; break; }
             OP_BC; break;
    case 17: // sc: bit 30 set, every other non-opcode bit reserved-zero
             if ((iw & ~0xFC000002u) == 0 && (iw & 2u)) { OP_SC; } else { OP_ILLEGAL; }
             break;
    case 18: OP_B; break;

    case 19: // branch/CR-logical group
        switch (PPC_XO10(iw)) {
        case 0:   OP_MCRF; break;
        case 16:  if (!ppc_bo_valid(PPC_RT(iw))) { OP_ILLEGAL; break; }
                  OP_BCLR; break;
        case 33:  OP_CRNOR; break;
        case 50:  OP_RFI; break;
        case 129: OP_CRANDC; break;
        case 150: OP_ISYNC; break;
        case 193: OP_CRXOR; break;
        case 225: OP_CRNAND; break;
        case 257: OP_CRAND; break;
        case 289: OP_CREQV; break;
        case 417: OP_CRORC; break;
        case 449: OP_CROR; break;
        case 528: if (!ppc_bo_valid(PPC_RT(iw))) { OP_ILLEGAL; break; }
                  OP_BCCTR; break;
        default:  OP_ILLEGAL; break;
        }
        break;

    case 20: OP_RLWIMI; break;
    case 21: OP_RLWINM; break;
    case 22: OP_RLMI; break;                                   // POWER
    case 23: OP_RLWNM; break;
    case 24: OP_ORI; break;
    case 25: OP_ORIS; break;
    case 26: OP_XORI; break;
    case 27: OP_XORIS; break;
    case 28: OP_ANDI_DOT; break;
    case 29: OP_ANDIS_DOT; break;

    case 31:
        switch (PPC_XO10(iw)) {

        // --- compares / traps ---
        case 0:   OP_CMP; break;
        case 32:  OP_CMPL; break;
        case 4:   OP_TW; break;

        // --- XO-form arithmetic (OE variant is xo+512; the RT_RA rows
        //     have a reserved RB field) ---
        case 266: case 266 + 512: OP_ADD; break;
        case 10:  case 10 + 512:  OP_ADDC; break;
        case 138: case 138 + 512: OP_ADDE; break;
        case 234: case 234 + 512: if (PPC_RB(iw)) { OP_ILLEGAL; break; }
                                  OP_ADDME; break;
        case 202: case 202 + 512: if (PPC_RB(iw)) { OP_ILLEGAL; break; }
                                  OP_ADDZE; break;
        case 40:  case 40 + 512:  OP_SUBF; break;
        case 8:   case 8 + 512:   OP_SUBFC; break;
        case 136: case 136 + 512: OP_SUBFE; break;
        case 232: case 232 + 512: if (PPC_RB(iw)) { OP_ILLEGAL; break; }
                                  OP_SUBFME; break;
        case 200: case 200 + 512: if (PPC_RB(iw)) { OP_ILLEGAL; break; }
                                  OP_SUBFZE; break;
        case 104: case 104 + 512: if (PPC_RB(iw)) { OP_ILLEGAL; break; }
                                  OP_NEG; break;
        case 75:  OP_MULHW; break;
        case 11:  OP_MULHWU; break;
        case 235: case 235 + 512: OP_MULLW; break;
        case 491: case 491 + 512: OP_DIVW; break;
        case 459: case 459 + 512: OP_DIVWU; break;

        // --- POWER arithmetic holdovers ---
        case 360: case 360 + 512: if (PPC_RB(iw)) { OP_ILLEGAL; break; }
                                  OP_ABS; break;
        case 488: case 488 + 512: if (PPC_RB(iw)) { OP_ILLEGAL; break; }
                                  OP_NABS; break;
        case 264: case 264 + 512: OP_DOZ; break;
        case 107: case 107 + 512: OP_MUL; break;
        case 331: case 331 + 512: OP_DIV; break;
        case 363: case 363 + 512: OP_DIVS; break;
        case 531: if (PPC_RB(iw)) { OP_ILLEGAL; break; }
                  OP_CLCS; break;

        // --- logical ---
        case 28:  OP_AND; break;
        case 60:  OP_ANDC; break;
        case 444: OP_OR; break;
        case 412: OP_ORC; break;
        case 316: OP_XOR; break;
        case 476: OP_NAND; break;
        case 124: OP_NOR; break;
        case 284: OP_EQV; break;
        case 954: OP_EXTSB; break;
        case 922: OP_EXTSH; break;
        case 26:  OP_CNTLZW; break;

        // --- architectural shifts ---
        case 24:  OP_SLW; break;
        case 536: OP_SRW; break;
        case 792: OP_SRAW; break;
        case 824: OP_SRAWI; break;

        // --- POWER shift-with-MQ family ---
        case 29:  OP_MASKG; break;
        case 541: OP_MASKIR; break;
        case 537: OP_RRIB; break;
        case 153: OP_SLE; break;
        case 217: OP_SLEQ; break;
        case 184: OP_SLIQ; break;
        case 248: OP_SLLIQ; break;
        case 216: OP_SLLQ; break;
        case 152: OP_SLQ; break;
        case 664: OP_SRQ; break;
        case 665: OP_SRE; break;
        case 696: OP_SRIQ; break;
        case 728: OP_SRLQ; break;
        case 729: OP_SREQ; break;
        case 760: OP_SRLIQ; break;
        case 920: OP_SRAQ; break;
        case 952: OP_SRAIQ; break;
        case 921: OP_SREA; break;

        // --- CR / MSR / SPR / SR moves ---
        case 19:  OP_MFCR; break;
        case 144: OP_MTCRF; break;
        case 512: OP_MCRXR; break;
        case 339: OP_MFSPR; break;
        case 467: OP_MTSPR; break;
        case 83:  OP_MFMSR; break;
        case 146: OP_MTMSR; break;
        case 595: OP_MFSR; break;
        case 210: OP_MTSR; break;
        case 659: OP_MFSRIN; break;
        case 242: OP_MTSRIN; break;
        case 306: OP_TLBIE; break;
        // xo 371 (mftb) deliberately absent: the 601 has RTC SPRs instead
        // of the timebase and traps mftb as illegal (601UM mfspr page).

        // --- storage control (RT and Rc are reserved-zero) ---
        case 598: OP_SYNC; break;
        case 854: OP_EIEIO; break;
        case 982: if (PPC_RT(iw) || PPC_RC(iw)) { OP_ILLEGAL; break; }
                  OP_ICBI; break;
        case 278: if (PPC_RT(iw) || PPC_RC(iw)) { OP_ILLEGAL; break; }
                  OP_DCBT; break;
        case 246: if (PPC_RT(iw) || PPC_RC(iw)) { OP_ILLEGAL; break; }
                  OP_DCBTST; break;
        case 54:  if (PPC_RT(iw) || PPC_RC(iw)) { OP_ILLEGAL; break; }
                  OP_DCBST; break;
        case 86:  if (PPC_RT(iw) || PPC_RC(iw)) { OP_ILLEGAL; break; }
                  OP_DCBF; break;
        case 470: if (PPC_RT(iw) || PPC_RC(iw)) { OP_ILLEGAL; break; }
                  OP_DCBI; break;
        case 1014: if (PPC_RT(iw) || PPC_RC(iw)) { OP_ILLEGAL; break; }
                  OP_DCBZ; break;

        // --- indexed loads/stores (Rc reserved-zero except stwcx.) ---
        case 23:  if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LWZX; break;
        case 55:  if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LWZUX; break;
        case 87:  if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LBZX; break;
        case 119: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LBZUX; break;
        case 279: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LHZX; break;
        case 311: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LHZUX; break;
        case 343: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LHAX; break;
        case 375: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LHAUX; break;
        case 151: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_STWX; break;
        case 183: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_STWUX; break;
        case 215: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_STBX; break;
        case 247: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_STBUX; break;
        case 407: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_STHX; break;
        case 439: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_STHUX; break;
        case 534: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LWBRX; break;
        case 790: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LHBRX; break;
        case 662: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_STWBRX; break;
        case 918: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_STHBRX; break;
        case 20:  if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LWARX; break;
        case 150: if (!PPC_RC(iw)) { OP_ILLEGAL; break; } OP_STWCX_DOT; break;
        case 310: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_ECIWX; break;
        case 438: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_ECOWX; break;

        // --- strings (lscbx has a record form) ---
        case 597: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LSWI; break;
        case 533: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LSWX; break;
        case 725: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_STSWI; break;
        case 661: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_STSWX; break;
        case 277: OP_LSCBX; break;

        // --- FP indexed loads/stores ---
        case 535: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LFSX; break;
        case 567: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LFSUX; break;
        case 599: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LFDX; break;
        case 631: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_LFDUX; break;
        case 663: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_STFSX; break;
        case 695: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_STFSUX; break;
        case 727: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_STFDX; break;
        case 759: if (PPC_RC(iw)) { OP_ILLEGAL; break; } OP_STFDUX; break;

        default:  OP_ILLEGAL; break;
        }
        break;

    // --- D-form loads/stores ---
    case 32: OP_LWZ; break;
    case 33: OP_LWZU; break;
    case 34: OP_LBZ; break;
    case 35: OP_LBZU; break;
    case 36: OP_STW; break;
    case 37: OP_STWU; break;
    case 38: OP_STB; break;
    case 39: OP_STBU; break;
    case 40: OP_LHZ; break;
    case 41: OP_LHZU; break;
    case 42: OP_LHA; break;
    case 43: OP_LHAU; break;
    case 44: OP_STH; break;
    case 45: OP_STHU; break;
    case 46: OP_LMW; break;
    case 47: OP_STMW; break;
    case 48: OP_LFS; break;
    case 49: OP_LFSU; break;
    case 50: OP_LFD; break;
    case 51: OP_LFDU; break;
    case 52: OP_STFS; break;
    case 53: OP_STFSU; break;
    case 54: OP_STFD; break;
    case 55: OP_STFDU; break;

    case 59: // FP single-precision arithmetic (A-form; the s-suffix group).
             // fdivs/fsubs/fadds carry a reserved FRC field; fmuls a
             // reserved FRB field.
        switch (PPC_XO5(iw)) {
        case 18: if (PPC_FRC(iw)) { OP_ILLEGAL; break; } OP_FDIVS; break;
        case 20: if (PPC_FRC(iw)) { OP_ILLEGAL; break; } OP_FSUBS; break;
        case 21: if (PPC_FRC(iw)) { OP_ILLEGAL; break; } OP_FADDS; break;
        case 25: if (PPC_RB(iw)) { OP_ILLEGAL; break; } OP_FMULS; break;
        case 28: OP_FMSUBS; break;
        case 29: OP_FMADDS; break;
        case 30: OP_FNMSUBS; break;
        case 31: OP_FNMADDS; break;
        default: OP_ILLEGAL; break;
        }
        break;

    case 63: // FP double group: X/XFL forms on the 10-bit xo, then A-form
             // arithmetic on the 5-bit xo
        switch (PPC_XO10(iw)) {
        case 0:   OP_FCMPU; break;
        case 32:  OP_FCMPO; break;
        case 72:  OP_FMR; break;
        case 40:  OP_FNEG; break;
        case 264: OP_FABS; break;
        case 136: OP_FNABS; break;
        case 583: OP_MFFS; break;
        case 711: OP_MTFSF; break;
        case 134: OP_MTFSFI; break;
        case 70:  OP_MTFSB0; break;
        case 38:  OP_MTFSB1; break;
        case 64:  OP_MCRFS; break;
        case 12:  OP_FRSP; break;
        case 14:  OP_FCTIW; break;
        case 15:  OP_FCTIWZ; break;
        default:
            switch (PPC_XO5(iw)) {
            case 18: if (PPC_FRC(iw)) { OP_ILLEGAL; break; } OP_FDIV; break;
            case 20: if (PPC_FRC(iw)) { OP_ILLEGAL; break; } OP_FSUB; break;
            case 21: if (PPC_FRC(iw)) { OP_ILLEGAL; break; } OP_FADD; break;
            case 25: if (PPC_RB(iw)) { OP_ILLEGAL; break; } OP_FMUL; break;
            case 28: OP_FMSUB; break;
            case 29: OP_FMADD; break;
            case 30: OP_FNMSUB; break;
            case 31: OP_FNMADD; break;
            default: OP_ILLEGAL; break;
            }
            break;
        }
        break;

    default: OP_ILLEGAL; break;
    }

    PPC_DECODER_EPILOGUE;
}

// clang-format on
