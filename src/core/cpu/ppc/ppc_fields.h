// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_fields.h
// PowerPC instruction-field accessors, in BE bit numbering per the 601UM
// chapter-10 diagrams.
//
// These sixteen macros had two byte-identical copies -- one in ppc_internal.h
// for the emulator, one at the top of ppc_disasm.c for the dependency-free
// disassembler TU that tools/disasm builds standalone.  A comment in
// ppc_internal.h claimed they were "kept in sync with the #ifndef-guarded copy
// in ppc_decode.h", which was wrong twice over: the copy was in ppc_disasm.c,
// and it carried no guard.  Nothing enforced the duplication either way.
//
// This header deliberately depends on nothing but <stdint.h>, so it can be
// included from the standalone disassembler build as well as the emulator.
#ifndef PPC_FIELDS_H
#define PPC_FIELDS_H

#include <stdint.h>

#define PPC_OPCD(iw) ((iw) >> 26)
#define PPC_RT(iw)   (((iw) >> 21) & 31) // also RS, TO, BO, crfD<<2|..
#define PPC_RA(iw)   (((iw) >> 16) & 31) // also BI
#define PPC_RB(iw)   (((iw) >> 11) & 31) // also SH, NB
#define PPC_XO10(iw) (((iw) >> 1) & 0x3FF) // X/XL/XFX-form extended opcode
#define PPC_XO9(iw)  (((iw) >> 1) & 0x1FF) // XO-form (bit 21 = OE)
#define PPC_XO5(iw)  (((iw) >> 1) & 0x1F) // A-form (FP arithmetic)
#define PPC_OE(iw)   (((iw) >> 10) & 1)
#define PPC_RC(iw)   ((iw) & 1)
#define PPC_SIMM(iw) ((int32_t)(int16_t)(iw))
#define PPC_UIMM(iw) ((iw) & 0xFFFFu)
#define PPC_MB(iw)   (((iw) >> 6) & 31)
#define PPC_ME(iw)   (((iw) >> 1) & 31)
#define PPC_FRC(iw)  (((iw) >> 6) & 31) // A-form third operand
#define PPC_CRFD(iw) (((iw) >> 23) & 7)
#define PPC_CRFS(iw) (((iw) >> 18) & 7)

#endif // PPC_FIELDS_H
