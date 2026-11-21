// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu_movec.h
// The MOVEC halves the 68030 and 68040 share: the extension-word decode and
// the control registers both models implement the same way (SFC, DFC, USP,
// VBR, MSP, ISP).  Each core's cpu_movec*_rc_rn / _rn_rc handles these
// first and keeps switch arms only for its own registers (CACR masks, the
// 030's CAAR, the 040's MMU registers).  Included by cpu_68030.c and
// cpu_68040.c after cpu_internal.h; MOVEC is supervisor-only and rare, so
// nothing here is on a hot path.

#ifndef CPU_MOVEC_H
#define CPU_MOVEC_H

#include "cpu_internal.h"

// A MOVEC extension word: the general register it names and the 12-bit
// control-register code
typedef struct {
    uint32_t *rn; // D0-D7 or A0-A7 (bit 15 selects)
    uint16_t rc;
} movec_ext_t;

// Fetches and decodes the MOVEC extension word
static inline movec_ext_t movec_ext(cpu_t *cpu) {
    uint16_t ext = fetch_16(cpu, true);
    uint32_t rn = (ext >> 12) & 7u;
    movec_ext_t x = {(ext & 0x8000u) ? &cpu->a[rn] : &cpu->d[rn], (uint16_t)(ext & 0x0FFFu)};
    return x;
}

// Reads a control register common to the 030 and 040 into *val; false when
// rc is not one of them.  MSP and ISP read whichever of A7 and the shadow
// register holds them under the current M bit.
static inline bool movec_common_read(const cpu_t *cpu, uint16_t rc, uint32_t *val) {
    switch (rc) {
    case 0x000:
        *val = cpu->sfc;
        return true;
    case 0x001:
        *val = cpu->dfc;
        return true;
    case 0x800:
        *val = cpu->usp;
        return true;
    case 0x801:
        *val = cpu->vbr;
        return true;
    case 0x803:
        *val = cpu->m ? cpu->a[7] : cpu->msp;
        return true;
    case 0x804:
        *val = cpu->m ? cpu->ssp : cpu->a[7];
        return true;
    default:
        return false;
    }
}

// Writes a control register common to the 030 and 040; false when rc is not
// one of them.  SFC/DFC keep their three function-code bits.
static inline bool movec_common_write(cpu_t *cpu, uint16_t rc, uint32_t val) {
    switch (rc) {
    case 0x000:
        cpu->sfc = val & 7u;
        return true;
    case 0x001:
        cpu->dfc = val & 7u;
        return true;
    case 0x800:
        cpu->usp = val;
        return true;
    case 0x801:
        cpu->vbr = val;
        return true;
    case 0x803:
        if (cpu->m)
            cpu->a[7] = val;
        else
            cpu->msp = val;
        return true;
    case 0x804:
        if (!cpu->m)
            cpu->a[7] = val;
        else
            cpu->ssp = val;
        return true;
    default:
        return false;
    }
}

#endif // CPU_MOVEC_H
