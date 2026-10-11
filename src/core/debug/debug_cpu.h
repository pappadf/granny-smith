// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug_cpu.h
// The main CPU as the debugger sees it, without any architecture's CPU
// struct: its privilege state, its registers by name, and an address
// translated the way its MMU would (side-effect-free).  Split from
// addr_format.h so that pure address parsing/formatting carries no MMU or
// CPU dependency.

#ifndef DEBUG_CPU_H
#define DEBUG_CPU_H

#include <stdbool.h>
#include <stdint.h>

// Whether the main CPU is in supervisor state, asked through the debug
// interface (true when no machine is live).
bool debug_cpu_is_supervisor(void);

// Resolve a 68K register name (pc, sp, ssp, usp, d0-d7, a0-a7; any case) to
// its current value.  Returns false when `name` is not a register or no 68K
// CPU is live.
bool debug_cpu_register_value(const char *name, uint32_t *value);

// Whether address translation is currently on (68030/68040 PMMU enabled).
bool debug_cpu_mmu_enabled(void);

// Translate a logical address to physical for debug display, without
// touching machine state (no MMUSR update, no ATC/SoA fill).
// Returns the physical address.  If no MMU or MMU disabled, returns logical_addr.
// Sets *is_identity to true if logical == physical.
// Sets *tt_hit to true if the address matched a transparent translation register.
// Sets *valid to true if translation succeeded.
// Any out-pointer may be NULL.
uint32_t debug_translate_address(uint32_t logical_addr, bool *is_identity, bool *tt_hit, bool *valid);

#endif // DEBUG_CPU_H
