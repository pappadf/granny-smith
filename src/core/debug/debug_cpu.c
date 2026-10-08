// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug_cpu.c
// The main CPU as the debugger sees it: privilege, registers by name, and
// side-effect-free address translation (see debug_cpu.h).

#include "debug_cpu.h"

#include "cpu.h"
#include "lisa_mmu.h"
#include "mmu.h"
#include "system.h"

#include <ctype.h>
#include <strings.h>

bool debug_cpu_is_supervisor(void) {
    const cpu_debug_if_t *dif = system_cpu_debug_if();
    return (dif && dif->is_supervisor) ? dif->is_supervisor(dif->ctx) : true;
}

// Returns the register number n for "<letter>n" (n in 0..7), or -1
static int numbered_register(const char *name, char letter) {
    if (tolower((unsigned char)name[0]) != letter)
        return -1;
    if (name[1] < '0' || name[1] > '7' || name[2] != '\0')
        return -1;
    return name[1] - '0';
}

bool debug_cpu_register_value(const char *name, uint32_t *value) {
    cpu_t *cpu = system_cpu();
    if (!cpu || !name || !value)
        return false;

    // PC, SP and the two stack pointers (names are case-insensitive)
    if (strcasecmp(name, "pc") == 0) {
        *value = cpu_get_pc(cpu);
        return true;
    }
    if (strcasecmp(name, "sp") == 0) {
        *value = cpu_get_an(cpu, 7);
        return true;
    }
    if (strcasecmp(name, "ssp") == 0) {
        *value = cpu_get_ssp(cpu);
        return true;
    }
    if (strcasecmp(name, "usp") == 0) {
        *value = cpu_get_usp(cpu);
        return true;
    }

    // D0-D7, then A0-A7
    int n = numbered_register(name, 'd');
    if (n >= 0) {
        *value = cpu_get_dn(cpu, n);
        return true;
    }
    n = numbered_register(name, 'a');
    if (n >= 0) {
        *value = cpu_get_an(cpu, n);
        return true;
    }

    return false;
}

bool debug_cpu_mmu_enabled(void) {
    return g_mmu && g_mmu->enabled;
}

uint32_t debug_translate_address(uint32_t logical_addr, bool *is_identity, bool *tt_hit, bool *valid) {
    if (is_identity)
        *is_identity = true;
    if (tt_hit)
        *tt_hit = false;
    if (valid)
        *valid = true;

    // The Lisa's segment MMU: its own translation, not the PMMU's (before,
    // this reported every Lisa address as mapped to itself).
    if (g_lisa_mmu) {
        uint32_t phys = logical_addr;
        bool ok = lisa_mmu_translate(g_lisa_mmu, logical_addr, debug_cpu_is_supervisor(), &phys, NULL);
        if (valid)
            *valid = ok;
        if (is_identity)
            *is_identity = ok && phys == logical_addr;
        return ok ? phys : logical_addr;
    }

    // No MMU or MMU disabled: identity mapping
    if (!g_mmu || !g_mmu->enabled)
        return logical_addr;

    // Translate via the current CPU mode rather than hardcoded supervisor so
    // that under TC.SRE=1 (separate user/supervisor roots) addresses dumped
    // while user code is running resolve through CRP, not SRP.  Also affects
    // breakpoint physical-page matching via the debug_check_pc_break caller.
    bool supervisor = debug_cpu_is_supervisor();

    // Check transparent translation first
    if (mmu_check_tt(g_mmu, logical_addr, false, supervisor)) {
        if (tt_hit)
            *tt_hit = true;
        // TT = identity mapping
        return logical_addr;
    }

    // One side-effect-free walk (68030 table walk or 68040 tree search)
    // yields both validity and the physical address
    uint32_t phys_addr = logical_addr;
    if (!mmu_translate_checked(g_mmu, logical_addr, supervisor, &phys_addr)) {
        // Invalid descriptor or bus error during the walk
        if (valid)
            *valid = false;
        return logical_addr;
    }

    if (is_identity)
        *is_identity = (phys_addr == logical_addr);

    return phys_addr;
}
