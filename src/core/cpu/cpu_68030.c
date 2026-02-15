// SPDX-License-Identifier: MIT
// cpu_68030.c
// Motorola 68030 instruction decoder stub.
// This file will instantiate the decoder template with 68030-specific macros
// (MMU-translated memory access, 68030 instructions like PMOVE/PFLUSH).
//
// For now this is a placeholder — the Plus (68000) is the only fully
// implemented machine. The IIcx (68030) will need:
//
// 1. Memory access macros that go through the page table (same inline path
//    as 68000, but page table may be rebuilt at runtime by MMU):
//    #define READ8(addr)   memory_read_uint8(addr)   // same fast path
//    #define READ16(addr)  memory_read_uint16(addr)
//    #define READ32(addr)  memory_read_uint32(addr)
//    #define WRITE8(addr, x)  memory_write_uint8(addr, x)
//    #define WRITE16(addr, x) memory_write_uint16(addr, x)
//    #define WRITE32(addr, x) memory_write_uint32(addr, x)
//
// 2. Additional 68030-specific instruction macros in cpu_ops.h overrides:
//    - OP_PMOVE: read/write MMU registers, trigger page table rebuild
//    - OP_PFLUSH: flush ATC entries, trigger page table rebuild
//    - OP_PTEST: test address translation (sets MMUSR)
//    - OP_CALLM/OP_RTM: module call/return (rarely used)
//
// 3. Decoder instantiation:
//    #define CPU_DECODER_NAME cpu_run_68030
//    #define CPU_DECODER_ARGS cpu_t *restrict cpu, uint32_t *instructions
//    #include "cpu_ops.h"
//    #include "cpu_decode.h"
//
// 4. The 68030 extends the 68000 ISA with:
//    - Bit field instructions (BFCHG, BFCLR, BFEXTS, BFEXTU, BFFFO, BFINS, BFSET, BFTST)
//    - Coprocessor interface (cpBcc, cpDBcc, cpGEN, cpRESTORE, cpSAVE, cpScc, cpTRAPcc)
//    - MMU instructions (PMOVE, PFLUSH, PTEST)
//    - CAS, CAS2 (compare and swap)
//    - CHK2, CMP2 (compare with bounds)
//    - PACK, UNPK (BCD conversion)

#include "cpu_internal.h"

// Stub: cpu_run_68030 is not yet implemented
void cpu_run_68030(cpu_t *restrict cpu, uint32_t *instructions) {
    (void)cpu;
    (void)instructions;
    // TODO: Instantiate decoder template with 68030-specific macros
    // For now, this is unreachable — only Plus (68000) is supported
}
