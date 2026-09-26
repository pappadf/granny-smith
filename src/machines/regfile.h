// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// regfile.h
// Big-endian byte-lane access for chipset register files.

#ifndef MACHINES_REGFILE_H
#define MACHINES_REGFILE_H

#include <stdint.h>

// Six chipsets keep 32-bit registers that the guest reaches a byte at a time,
// and each had grown its own lane extract and insert: oss.c's be32_byte,
// psc.c's lane32 (plus a second inline copy), amic.c's addr_read_byte /
// addr_write_byte, and open-coded shifts in hammerhead.c, mcu.c and av.c --
// three naming conventions and four spellings of the same shift
// (`3u - (index & 3u)`, `3 - (off & 3)`, `3 - lane`, `3 - reg_off`).
// All of them now use this header.
//
// LANE 0 IS THE MOST SIGNIFICANT BYTE.  That is the convention on this bus
// and it is the whole reason the shift is `3 - lane` rather than `lane`; it
// had to be re-derived at every site before this header existed.
//
// These are `static inline` rather than macros, which is the opposite of the
// RD_BE*/WR_BE* decision in common.h -- those are macros because at -Og the
// memcpy-based loads were not reliably inlined and cost 9 instructions against
// 2.  Measured for these: a lane extract compiles to 4 instructions as an
// inline function and 4 as a macro, at both -Og and -O2, because it is plain
// scalar arithmetic with nothing for the inliner to decline.  So the typed
// form wins at no cost.

// Extract byte `lane` (0 = MSB) from a big-endian 32-bit register value.
static inline uint8_t be_lane8(uint32_t value, unsigned lane) {
    return (uint8_t)(value >> ((3u - (lane & 3u)) * 8u));
}

// Replace byte `lane` (0 = MSB) of a big-endian 32-bit register value.
static inline void be_lane8_set(uint32_t *value, unsigned lane, uint8_t byte) {
    unsigned shift = (3u - (lane & 3u)) * 8u;
    *value = (*value & ~(0xFFu << shift)) | ((uint32_t)byte << shift);
}

#endif // MACHINES_REGFILE_H
