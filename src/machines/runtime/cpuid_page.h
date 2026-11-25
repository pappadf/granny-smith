// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpuid_page.h
// The CPU-ID register page of the 68030/040 boards that carry one: the page
// at $5FFFF000 whose last longword ($5FFFFFFC) is the read-only machine-ID
// register ($A55Axxxx: Apple's signature, then the design centre and id).
// The ROM's GetCPUIDReg validates the signature and that the location is not
// writable, so writes are dropped and the write-then-readback probe sees the
// constant.  The rest of the page reads `fill` (nothing drives the bus).
//
// The AV and djMEMC/MEMCjr boards install it; it is written so the other
// boards with the same page (V8/Sonora, the PDM HMC) can adopt it: their
// differences -- a zero fill, a long read that drops the signature -- are
// fields here.

#ifndef GS_MACHINES_RUNTIME_CPUID_PAGE_H
#define GS_MACHINES_RUNTIME_CPUID_PAGE_H

#include "memory.h"

#include <stdbool.h>
#include <stdint.h>

#define CPUID_PAGE_BASE 0x5FFFF000u // the page; the register is its last longword
#define CPUID_PAGE_REG  0xFFCu // the register's offset within the page

// One board's CPU-ID page: its register value and the device face the memory
// map routes the page to.
typedef struct cpuid_page {
    uint32_t value; // the register's value
    uint8_t fill; // what the rest of the page reads ($FF: floats high)
    bool long_low_half; // a 32-bit read of the register returns only its low half (PDM)
    memory_interface_t iface; // registered at CPUID_PAGE_BASE
} cpuid_page_t;

// Fill in `pg` (`fill` $FF, full-width long reads) and register its page at
// CPUID_PAGE_BASE in `map` as "CPU-ID".  `pg` must outlive the map.
void cpuid_page_install(cpuid_page_t *pg, memory_map_t *map, uint32_t value);

#endif // GS_MACHINES_RUNTIME_CPUID_PAGE_H
