// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_decode.h
// Instruction decoder for MC68000 opcodes.
// Note: this header is a template intended for multiple inclusion with different
// macro parameters; it intentionally has no include guard.

// Required macro configuration (provided by includer):
// - CPU_DECODER_NAME:     Symbol/name of the generated decoder function
// - CPU_DECODER_RETURN_TYPE: Return type of the decoder function
// - CPU_DECODER_ARGS:     Formal parameter list for the decoder (e.g., uint16_t opcode)
// - CPU_DECODER_PROLOGUE: Code emitted at function start (e.g., prefetch, locals)
// - CPU_DECODER_EPILOGUE: Code emitted before returning (e.g., commit/writeback)

// clang-format off

#ifndef CPU_DECODER_NAME
#define CPU_DECODER_NAME cpu_decoder_default
#error "Decoder name macro not defined"
#endif

#if !defined(CPU_DECODER_RETURN_TYPE)
#define CPU_DECODER_RETURN_TYPE void
#error "Decoder return type macro not defined"
#endif

#if !defined(CPU_DECODER_ARGS)
#define CPU_DECODER_ARGS uint16_t opcode
#error "Decoder args macro not defined"
#endif

#if !defined(CPU_DECODER_PROLOGUE)
#define CPU_DECODER_PROLOGUE assert(0)
#error "Decoder prologue macro not defined"
#endif

#if !defined(CPU_DECODER_EPILOGUE)
#define CPU_DECODER_EPILOGUE assert(0)
#error "Decoder epilogue macro not defined"
#endif

// Top-level opcode decode routine: dispatches based on the top 4 bits, then
// refines using mid/low fields to select the concrete operation macro.
CPU_DECODER_RETURN_TYPE CPU_DECODER_NAME(CPU_DECODER_ARGS) {

    CPU_DECODER_PROLOGUE;

    // Dispatch by top 4 opcode bits

    switch (opcode >> 12) {

    case 0x0: // 0000.xxxx.xxxx.xxxx
        switch (((opcode) >> 6) & 0x3F) {
        case 0x00: if (opcode == 0x003C) { OP_ORI_B_DATA_CCR; } else { OP_ORI_B_DATA_EA; } break;
        case 0x01: if (((opcode) & 0x3F) == 0x003C) { OP_ORI_W_DATA_SR;  } else { OP_ORI_W_DATA_EA;  } break;
        case 0x02: OP_ORI_L_DATA_EA; break;
        case 0x03: OP_CHK2_B_EA_DN;  break;

        case 0x08: if (((opcode) & 0x3F) == 0x003C) { OP_ANDI_B_DATA_CCR; } else { OP_ANDI_B_DATA_EA; } break;
        case 0x09: if (((opcode) & 0x3F) == 0x003C) { OP_ANDI_W_DATA_SR;  } else { OP_ANDI_W_DATA_EA;  } break;
        case 0x0A: OP_ANDI_L_DATA_EA; break;
        case 0x0B: OP_CHK2_W_EA_DN;  break;

        case 0x10: OP_SUBI_B_DATA_EA; break;
        case 0x11: OP_SUBI_W_DATA_EA; break;
        case 0x12: OP_SUBI_L_DATA_EA; break;
        case 0x13: OP_CHK2_L_EA_DN;  break;

        case 0x18: OP_ADDI_B_DATA_EA; break;
        case 0x19: OP_ADDI_W_DATA_EA; break;
        case 0x1A: OP_ADDI_L_DATA_EA; break;
        case 0x1B: if ((opcode & 0x30) == 0x00) { OP_RTM_RN; } else { OP_CALLM_DATA_EA; } break;

        case 0x20: if ((opcode & 0x38) == 0x00) { OP_BTST_L_DATA_DN; } else { OP_BTST_B_DATA_EA; } break;
        case 0x21: if ((opcode & 0x38) == 0x00) { OP_BCHG_L_DATA_DN; } else { OP_BCHG_B_DATA_EA; } break;
        case 0x22: if ((opcode & 0x38) == 0x00) { OP_BCLR_L_DATA_DN; } else { OP_BCLR_B_DATA_EA; } break;
        case 0x23: if ((opcode & 0x38) == 0x00) { OP_BSET_L_DATA_DN; } else { OP_BSET_B_DATA_EA; } break;

        case 0x28: if (opcode == 0x0A3C) { OP_EORI_B_DATA_CCR; } else { OP_EORI_B_DATA_EA; } break;
        case 0x29: if (opcode == 0x0A7C) { OP_EORI_W_DATA_SR;  } else { OP_EORI_W_DATA_EA;  } break;
        case 0x2A: OP_EORI_L_DATA_EA; break;
        case 0x2B: OP_CAS_B_DC_DU_EA; break;

        case 0x30: OP_CMPI_B_DATA_EA; break;
        case 0x31: OP_CMPI_W_DATA_EA; break;
        case 0x32: OP_CMPI_L_DATA_EA; break;
        case 0x33: if (((opcode) & 0x3F) == 0x003C) { OP_CAS2_W_DC_DU_RN; } else { OP_CAS_W_DC_DU_EA; } break;

        case 0x38: if (ext_word & 0x0800) { OP_MOVES_B_RN_EA; } else { OP_MOVES_B_EA_RN; } break;
        case 0x39: if (ext_word & 0x0800) { OP_MOVES_W_RN_EA; } else { OP_MOVES_W_EA_RN; } break;
        case 0x3A: if (ext_word & 0x0800) { OP_MOVES_L_RN_EA; } else { OP_MOVES_L_EA_RN; } break;
        case 0x3B: if (((opcode) & 0x3F) == 0x003C) { OP_CAS2_L_DC_DU_RN; } else { OP_CAS_L_DC_DU_EA; } break;

        default:
            switch ((opcode >> 6) & 7) {
            case 4:
                if      ((opcode & 0x38) == 0x08) { OP_MOVEP_W_D16AY_DX; }
                else if ((opcode & 0x38) == 0x00) { OP_BTST_L_DX_DY;    }
                else                              { OP_BTST_B_DN_EA;    }
                break;
            case 5:
                if      ((opcode & 0x38) == 0x08) { OP_MOVEP_L_D16AY_DX; }
                else if ((opcode & 0x38) == 0x00) { OP_BCHG_L_DX_DY;     }
                else                              { OP_BCHG_B_DN_EA;     }
                break;
            case 6:
                if      ((opcode & 0x38) == 0x08) { OP_MOVEP_W_DX_D16AY; }
                else if ((opcode & 0x38) == 0x00) { OP_BCLR_L_DX_DY;     }
                else                              { OP_BCLR_B_DN_EA;     }
                break;
            case 7:
                if      ((opcode & 0x38) == 0x08) { OP_MOVEP_L_DX_D16AY; }
                else if (((opcode >> 3) & 7) == 0){ OP_BSET_L_DX_DY;     }
                else                              { OP_BSET_B_DN_EA;     }
                break;
            default: OP_UNDEFINED;
            }
        }
        break;

    case 0x1: OP_MOVE_B_EA_EA; break;            // 0001.xxxx.xxxx.xxxx

    case 0x2: // 0010.xxxx.xxxx.xxxx
        if ((opcode & 0x01C0) == 0x0040) { OP_MOVEA_L_EA_AN; } else { OP_MOVE_L_EA_EA; }
        break;

    case 0x3: // 0011.xxxx.xxxx.xxxx
        if ((opcode & 0x01C0) == 0x0040) { OP_MOVEA_W_EA_AN; } else { OP_MOVE_W_EA_EA; }
        break;

    case 0x4: // 0100.xxxx.xxxx.xxxx
        switch (((opcode) >> 6) & 0x3F) {
        case 0x00: OP_NEGX_B_EA; break;
        case 0x01: OP_NEGX_W_EA; break;
        case 0x02: OP_NEGX_L_EA; break;
        case 0x03: OP_MOVE_W_SR_EA; break;

        case 0x08: OP_CLR_B_EA; break;
        case 0x09: OP_CLR_W_EA; break;
        case 0x0A: OP_CLR_L_EA; break;
        case 0x0B: OP_MOVE_B_CCR_EA; break;

        case 0x10: OP_NEG_B_EA; break;
        case 0x11: OP_NEG_W_EA; break;
        case 0x12: OP_NEG_L_EA; break;
        case 0x13: OP_MOVE_B_EA_CCR; break;

        case 0x18: OP_NOT_B_EA; break;
        case 0x19: OP_NOT_W_EA; break;
        case 0x1A: OP_NOT_L_EA; break;
        case 0x1B: OP_MOVE_EA_SR; break;

        case 0x20: if (((opcode) & 0xFFF8) == 0x4808) { OP_LINK_L_AN_DISP; } else { OP_NBCD_B_EA; } break;
        case 0x21:
            if      ((opcode & 0x38) == 0x00) { OP_SWAP_DN; }
            else if ((opcode & 0x38) == 0x08) { OP_BKPT_DATA; }
            else                              { OP_PEA_EA; }
            break;

        case 0x22: if (((opcode) & 0xFFF8) == 0x4880) { OP_EXT_W_DN; } else { OP_MOVEM_W_LIST_EA; } break;
        case 0x23: if ((opcode & 0x38) == 0x00) { OP_EXT_L_DN; } else { OP_MOVEM_L_LIST_EA; } break;

        case 0x28: OP_TST_B_EA; break;
        case 0x29: OP_TST_W_EA; break;
        case 0x2A: OP_TST_L_EA; break;
        case 0x2B: if (opcode == 0x4AFC) { OP_ILLEGAL; } else { OP_TAS_B_EA; } break;

        case 0x30: OP_MULS_L_EA_DH_DL; break;
        case 0x31: OP_DIVS_L_EA_DR_DQ; break;
        case 0x32: OP_MOVEM_W_EA_LIST;  break;
        case 0x33: OP_MOVEM_L_EA_LIST;  break;

        case 0x39:
            if ((opcode & 0xFFF0) == 0x4E40) { OP_TRAP_VECTOR; break; }
            switch (opcode) {
            case 0x4E70: OP_RESET; break;
            case 0x4E71: OP_NOP; break;
            case 0x4E72: OP_STOP_DATA; break;
            case 0x4E73: OP_RTE; break;
            case 0x4E74: OP_RTD_DISPLACEMENT; break;
            case 0x4E75: OP_RTS; break;
            case 0x4E76: OP_TRAPV; break;
            case 0x4E77: OP_RTR; break;
            case 0x4E7A: OP_MOVEC_RC_RN; break;
            case 0x4E7B: OP_MOVEC_RN_RC; break;
            default:
                switch (opcode & 0xFFF8) {
                case 0x4E50: OP_LINK;         break;
                case 0x4E58: OP_UNLK;         break;
                case 0x4E60: OP_MOVE_AN_USP;  break;
                case 0x4E68: OP_MOVE_USP_AN;  break;
                default:     OP_UNDEFINED;    break;
                }
                break;
            }
            break;

        case 0x3A: OP_JSR_EA; break;
        case 0x3B: OP_JMP_EA; break;

    // Grouped cases
        case 0x07: case 0x0F: case 0x17: case 0x1F:
        case 0x27: case 0x2F: case 0x37: case 0x3F: OP_LEA_EA_AN;    break;
        case 0x06: case 0x0E: case 0x16: case 0x1E:
        case 0x26: case 0x2E: case 0x36: case 0x3E: OP_CHK_W_EA_DN;  break;
        case 0x04: case 0x0C: case 0x14: case 0x1C:
        case 0x24: case 0x2C: case 0x34: case 0x3C: OP_CHK_L_EA_DN;  break;

        default: OP_UNDEFINED; break;
        }
        break;

    case 0x5: // 0101.xxxx.xxxx.xxxx
        if ((opcode & 0x00F8) == 0x00C8) { OP_DBCC_DN_LABEL; break; }
        if ((opcode & 0x00C0) == 0x00C0) {
            switch (opcode & 0x003F) {
            case 0x3C: OP_TRAPCC;          break;
            case 0x3A: OP_TRAPCC_W_DATA;   break;
            case 0x3B: OP_TRAPCC_L_DATA;   break;
            default:   OP_SCC_EA;          break;
            }
            break;
        }
        switch (opcode & 0x01F8) {
        case 0x0148: OP_SUBQ_W_DATA_AN; break;
        case 0x0188: OP_SUBQ_L_DATA_AN; break;
        case 0x0048: OP_ADDQ_W_DATA_AN; break;
        case 0x0088: OP_ADDQ_L_DATA_AN; break;
        default:
            switch ((opcode) & 0x01C0) {
            case 0x0100: OP_SUBQ_B_DATA_EA; break;
            case 0x0140: OP_SUBQ_W_DATA_EA; break;
            case 0x0180: OP_SUBQ_L_DATA_EA; break;
            case 0x0000: OP_ADDQ_B_DATA_EA; break;
            case 0x0040: OP_ADDQ_W_DATA_EA; break;
            case 0x0080: OP_ADDQ_L_DATA_EA; break;
            default:     OP_UNDEFINED;      break;
            }
            break;
        }
        break;

    case 0x6: // 0110.xxxx.xxxx.xxxx
        // Note: 68000 only supports Bcc.B (8-bit) and Bcc.W (16-bit) displacements
        // The 0xFF case (32-bit displacement) is 68020+ only
        if ((((opcode) >> 8) & 0xF) == 0x1) {
            switch (opcode & 0x00FF) {
            case 0xFF: OP_BSR_L_LABEL; break; // todo: make this 68020+ only
            case 0x00: OP_BSR_W_LABEL; break;
            default:   OP_BSR_B_LABEL; break;
            }
        } else {
            switch (opcode & 0x00FF) {
            case 0xFF: OP_BCC_L_DISPLACEMENT; break; // todo: make this 68020+ only
            case 0x00: OP_BCC_W_DISPLACEMENT; break;
            default:   OP_BCC_B_DISPLACEMENT; break;
            }
        }
        break;

    case 0x7: // 0111.xxxx.xxxx.xxxx
        if ((opcode & 0x0100) == 0x0000) { OP_MOVEQ_L_DATA_DN; } else { OP_UNDEFINED; }
        break;

    case 0x8: // 1000.xxxx.xxxx.xxxx
        { const uint16_t mid5 = (opcode >> 4) & 0x1F;
          if (mid5 == 0x18) { if (opcode & 0x0008) { OP_UNPK_AY_AX; } else { OP_UNPK_DY_DX; } break; }
          if (mid5 == 0x14) { if (opcode & 0x0008) { OP_PACK_AY_AX; } else { OP_PACK_DY_DX; } break; }
        }
        switch ((opcode) & 0x01F8) {
        case 0x0100: OP_SBCD_DX_DY; break;
        case 0x0108: OP_SBCD_AX_AY; break;
        default:
            switch ((opcode) & 0x01C0) {
            case 0x0000: OP_OR_B_EA_DN;  break;
            case 0x0040: OP_OR_W_EA_DN;  break;
            case 0x0080: OP_OR_L_EA_DN;  break;
            case 0x00C0: OP_DIVU_W_EA_DN; break;
            case 0x0100: OP_OR_B_DN_EA;  break;
            case 0x0140: OP_OR_W_DN_EA;  break;
            case 0x0180: OP_OR_L_DN_EA;  break;
            case 0x01C0: OP_DIVS_W_EA_DN; break;
            default:     OP_UNDEFINED;   break;
            }
            break;
        }
        break;

    case 0x9: // 1001.xxxx.xxxx.xxxx
        switch ((opcode) & 0x01C0) {
        case 0x00C0: OP_SUBA_W_EA_AN; break;
        case 0x01C0: OP_SUBA_L_EA_AN; break;
        default:
            switch ((opcode) & 0x01F8) {
            case 0x0100: OP_SUBX_B_DX_DY; break;
            case 0x0140: OP_SUBX_W_DX_DY; break;
            case 0x0180: OP_SUBX_L_DX_DY; break;
            case 0x0108: OP_SUBX_B_AX_AY; break;
            case 0x0148: OP_SUBX_W_AX_AY; break;
            case 0x0188: OP_SUBX_L_AX_AY; break;
            default:
                switch ((opcode) & 0x01C0) {
                case 0x0000: OP_SUB_B_EA_DN; break;
                case 0x0040: OP_SUB_W_EA_DN; break;
                case 0x0080: OP_SUB_L_EA_DN; break;
                case 0x0100: OP_SUB_B_DN_EA; break;
                case 0x0140: OP_SUB_W_DN_EA; break;
                case 0x0180: OP_SUB_L_DN_EA; break;
                default:     OP_UNDEFINED;   break;
                }
                break;
            }
            break;
        }
        break;

    case 0xA: // 1010.xxxx.xxxx.xxxx (A-line)
        OP_ATRAP;
        break;

    case 0xB: // 1011.xxxx.xxxx.xxxx
        switch ((opcode) & 0x01F8) {
        case 0x0108: OP_CMPM_B_AY_AX; break;
        case 0x0148: OP_CMPM_W_AY_AX; break;
        case 0x0188: OP_CMPM_L_AY_AX; break;
        default:
            switch ((opcode) & 0x01C0) {
            case 0x00C0: OP_CMPA_W_EA_AN; break;
            case 0x01C0: OP_CMPA_L_EA_AN; break;
            case 0x0100: OP_EOR_B_DN_EA;  break;
            case 0x0140: OP_EOR_W_DN_EA;  break;
            case 0x0180: OP_EOR_L_DN_EA;  break;
            case 0x0000: OP_CMP_B_EA_DN;  break;
            case 0x0040: OP_CMP_W_EA_DN;  break;
            case 0x0080: OP_CMP_L_EA_DN;  break;
            default:     OP_UNDEFINED;    break;
            }
            break;
        }
        break;

    case 0xC: // 1100.xxxx.xxxx.xxxx
        switch ((opcode >> 6) & 7) {
        case 0: OP_AND_B_EA_DN;  break;
        case 1: OP_AND_W_EA_DN;  break;
        case 2: OP_AND_L_EA_DN;  break;
        case 3: OP_MULU_W_EA_DN; break;
        case 4:
            if      (((opcode) & 0x01F8) == 0x0100) { OP_ABCD_DY_DX; }
            else if (((opcode) & 0x01F8) == 0x0108) { OP_ABCD_AY_AX; }
            else                       { OP_AND_B_DN_EA; }
            break;
        case 5:
            if      (((opcode) & 0x01F8) == 0x0140) { OP_EXG_DX_DY; }
            else if (((opcode) & 0x01F8) == 0x0148) { OP_EXG_AX_AY; }
            else                       { OP_AND_W_DN_EA; }
            break;
        case 6:
            if      (((opcode) & 0x01F8) == 0x0188) { OP_EXG_DX_AY; }
            else                       { OP_AND_L_DN_EA; }
            break;
        case 7: OP_MULS_W_EA_DN; break;
        }
        break;

    case 0xD: // 1101.xxxx.xxxx.xxxx
        switch ((opcode >> 6) & 7) {
        case 0: OP_ADD_B_EA_DN;  break;
        case 1: OP_ADD_W_EA_DN;  break;
        case 2: OP_ADD_L_EA_DN;  break;
        case 3: OP_ADDA_W_EA_AN; break;
        case 4:
            if      (((opcode >> 3) & 0x3F) == 0x20) { OP_ADDX_B_DY_DX; }
            else if (((opcode >> 3) & 0x3F) == 0x21) { OP_ADDX_B_AY_AX; }
            else                                     { OP_ADD_B_DN_EA;  }
            break;
        case 5:
            if      (((opcode >> 3) & 0x3F) == 0x28) { OP_ADDX_W_DY_DX; }
            else if (((opcode >> 3) & 0x3F) == 0x29) { OP_ADDX_W_AY_AX; }
            else                                     { OP_ADD_W_DN_EA;  }
            break;
        case 6:
            if      (((opcode >> 3) & 0x3F) == 0x30) { OP_ADDX_L_DY_DX; }
            else if (((opcode >> 3) & 0x3F) == 0x31) { OP_ADDX_L_AY_AX; }
            else                                     { OP_ADD_L_DN_EA;  }
            break;
        case 7: OP_ADDA_L_EA_AN; break;
        }
        break;

    case 0xE: // 1110.xxxx.xxxx.xxxx
        // Bit-field unit first (020+), then classic shifts/rotates
        switch ((opcode) & 0x0FC0) {
        case 0x08C0: if ((((opcode) >> 3) & 7) == 0) { OP_BFTST_DN; }  else { OP_BFTST_EA; }  break;
        case 0x09C0: if ((((opcode) >> 3) & 7) == 0) { OP_BFEXTU_DN; } else { OP_BFEXTU_EA; } break;
        case 0x0AC0: if ((((opcode) >> 3) & 7) == 0) { OP_BFCHG_DN; }  else { OP_BFCHG_EA; }  break;
        case 0x0BC0: if ((((opcode) >> 3) & 7) == 0) { OP_BFEXTS_DN; } else { OP_BFEXTS_EA; } break;
        case 0x0CC0: if ((((opcode) >> 3) & 7) == 0) { OP_BFCLR_DN; }  else { OP_BFCLR_EA; }  break;
        case 0x0DC0: if ((((opcode) >> 3) & 7) == 0) { OP_BFFFO_DN; }  else { OP_BFFFO_EA; }  break;
        case 0x0EC0: if ((((opcode) >> 3) & 7) == 0) { OP_BFSET_DN; }  else { OP_BFSET_EA; }  break;
        case 0x0FC0: if ((((opcode) >> 3) & 7) == 0) { OP_BFINS_DN; }  else { OP_BFINS_EA; }  break;
        default:
            switch ((opcode >> 3) & 0x3F) {
            // Right shifts/rotates: immediate, then register forms
            case 0x00: OP_ASR_B_DATA_DY; break;
            case 0x01: OP_LSR_B_DATA_DY; break;
            case 0x02: OP_ROXR_B_DATA_DY; break;
            case 0x03: OP_ROR_B_DATA_DY;  break;
            case 0x04: OP_ASR_B_DX_DY; break;
            case 0x05: OP_LSR_B_DX_DY; break;
            case 0x06: OP_ROXR_B_DX_DY; break;
            case 0x07: OP_ROR_B_DX_DY;  break;
            case 0x08: OP_ASR_W_DATA_DY; break;
            case 0x09: OP_LSR_W_DATA_DY; break;
            case 0x0A: OP_ROXR_W_DATA_DY; break;
            case 0x0B: OP_ROR_W_DATA_DY;  break;
            case 0x0C: OP_ASR_W_DX_DY; break;
            case 0x0D: OP_LSR_W_DX_DY; break;
            case 0x0E: OP_ROXR_W_DX_DY; break;
            case 0x0F: OP_ROR_W_DX_DY;  break;
            case 0x10: OP_ASR_L_DATA_DY; break;
            case 0x11: OP_LSR_L_DATA_DY; break;
            case 0x12: OP_ROXR_L_DATA_DY; break;
            case 0x13: OP_ROR_L_DATA_DY;  break;
            case 0x14: OP_ASR_L_DX_DY; break;
            case 0x15: OP_LSR_L_DX_DY; break;
            case 0x16: OP_ROXR_L_DX_DY; break;
            case 0x17: OP_ROR_L_DX_DY;  break;

            // Memory forms (word size)
            case 0x18: case 0x19: case 0x1A: case 0x1B:
            case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                switch (opcode & 0x0E00) {
                case 0x0000: OP_ASR_W_EA;  break;
                case 0x0200: OP_LSR_W_EA;  break;
                case 0x0400: OP_ROXR_W_EA; break;
                case 0x0600: OP_ROR_W_EA;  break;
                default:     OP_UNDEFINED; break;
                }
                break;

            // Left shifts/rotates: immediate, then register forms
            case 0x20: OP_ASL_B_DATA_DY; break;
            case 0x21: OP_LSL_B_DATA_DY; break;
            case 0x22: OP_ROXL_B_DATA_DY; break;
            case 0x23: OP_ROL_B_DATA_DY;  break;
            case 0x24: OP_ASL_B_DX_DY; break;
            case 0x25: OP_LSL_B_DX_DY; break;
            case 0x26: OP_ROXL_B_DX_DY; break;
            case 0x27: OP_ROL_B_DX_DY;  break;
            case 0x28: OP_ASL_W_DATA_DY; break;
            case 0x29: OP_LSL_W_DATA_DY; break;
            case 0x2A: OP_ROXL_W_DATA_DY; break;
            case 0x2B: OP_ROL_W_DATA_DY;  break;
            case 0x2C: OP_ASL_W_DX_DY; break;
            case 0x2D: OP_LSL_W_DX_DY; break;
            case 0x2E: OP_ROXL_W_DX_DY; break;
            case 0x2F: OP_ROL_W_DX_DY;  break;
            case 0x30: OP_ASL_L_DATA_DY; break;
            case 0x31: OP_LSL_L_DATA_DY; break;
            case 0x32: OP_ROXL_L_DATA_DY; break;
            case 0x33: OP_ROL_L_DATA_DY;  break;
            case 0x34: OP_ASL_L_DX_DY; break;
            case 0x35: OP_LSL_L_DX_DY; break;
            case 0x36: OP_ROXL_L_DX_DY; break;
            case 0x37: OP_ROL_L_DX_DY;  break;

            // Memory forms (word size)
            case 0x38: case 0x39: case 0x3A: case 0x3B:
            case 0x3C: case 0x3D: case 0x3E: case 0x3F:
                switch (opcode & 0x0E00) {
                case 0x0000: OP_ASL_W_EA;  break;
                case 0x0200: OP_LSL_W_EA;  break;
                case 0x0400: OP_ROXL_W_EA; break;
                case 0x0600: OP_ROL_W_EA;  break;
                default:     OP_UNDEFINED; break;
                }
                break;
            }
            break;
        }
        break;

    case 0xF: // 1111.xxxx.xxxx.xxxx
        switch (((opcode) >> 6) & 0x3F) {
        case 0x02: if (((opcode) & 0x3F) < 0x10) { OP_PBCC_W; } else { OP_FTRAP; } break;
        case 0x03: if (((opcode) & 0x3F) < 0x10) { OP_PBCC_L; } else { OP_FTRAP; } break;
        case 0x04: OP_PSAVE_EA;     break;
        case 0x05: OP_PRESTORE_EA;  break;
        case 0x0A: if (((opcode) & 0x3F) < 0x20) { OP_FBCC_W_DISPLACEMENT; } else { OP_FTRAP; } break;
        case 0x0B: if (((opcode) & 0x3F) < 0x20) { OP_FBCC_L_DISPLACEMENT; } else { OP_FTRAP; } break;
        case 0x0C: OP_FSAVE_EA;     break;
        case 0x0D: OP_FRESTORE_EA;  break;

        case 0x10: case 0x11: case 0x12: case 0x13:
            switch ((opcode >> 3) & 7) {
            case 1: OP_CINVL_CACHES_AN;  break;
            case 2: OP_CINVP_CACHES_AN;  break;
            case 3: OP_CINVA_CACHES;     break;
            case 5: OP_CPUSHL_CACHES_AN; break;
            case 6: OP_CPUSHP_CACHES_AN; break;
            case 7: OP_CPUSHA_CACHES;    break;
            default: OP_FTRAP;           break;
            }
            break;

        case 0x14:
            switch ((opcode >> 3) & 7) {
            case 0: OP_PFLUSHN_AN; break;
            case 1: OP_PFLUSH_AN;  break;
            case 2: OP_PFLUSHAN;   break;
            case 3: OP_PFLUSHA;    break;
            default: OP_FTRAP;     break;
            }
            break;

        case 0x15:
            switch ((opcode >> 3) & 7) {
            case 1: OP_PTESTW_AN; break;
            case 5: OP_PTESTR_AN; break;
            default: OP_FTRAP;    break;
            }
            break;

        case 0x18:
            switch ((opcode >> 3) & 7) {
            case 0: OP_MOVE16_AN_P_XXX_L; break;
            case 1: OP_MOVE16_XXX_L_AN_P; break;
            case 2: OP_MOVE16_AN_XXX_L;   break;
            case 3: OP_MOVE16_XXX_L_AN;   break;
            default: OP_FTRAP;            break;
            }
            break;

        default:
            OP_FTRAP;
            break;
        }
        break;
    }

    CPU_DECODER_EPILOGUE;
}
