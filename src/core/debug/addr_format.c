// SPDX-License-Identifier: MIT
// addr_format.c
// Unified address parsing and formatting for logical/physical addresses.

#include "addr_format.h"

#include "cpu.h"
#include "mmu.h"
#include "system.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Default display mode: auto (collapsed unless MMU is active)
addr_display_mode_t g_addr_display_mode = ADDR_DISPLAY_AUTO;

// Try to resolve a name as a CPU register, returning its value (IMP-407)
// Returns true if name matched a register, false otherwise.
static bool resolve_register_address(const char *name, uint32_t *value) {
    cpu_t *cpu = system_cpu();
    if (!cpu)
        return false;

    // Case-insensitive comparisons
    char lower[8];
    size_t len = strlen(name);
    if (len == 0 || len > 7)
        return false;
    for (size_t i = 0; i <= len; i++)
        lower[i] = tolower((unsigned char)name[i]);

    // PC, SP
    if (strcmp(lower, "pc") == 0) {
        *value = cpu_get_pc(cpu);
        return true;
    }
    if (strcmp(lower, "sp") == 0) {
        *value = cpu_get_an(cpu, 7);
        return true;
    }
    if (strcmp(lower, "ssp") == 0) {
        *value = cpu_get_ssp(cpu);
        return true;
    }
    if (strcmp(lower, "usp") == 0) {
        *value = cpu_get_usp(cpu);
        return true;
    }

    // D0-D7
    if (lower[0] == 'd' && lower[1] >= '0' && lower[1] <= '7' && lower[2] == '\0') {
        *value = cpu_get_dn(cpu, lower[1] - '0');
        return true;
    }

    // A0-A7
    if (lower[0] == 'a' && lower[1] >= '0' && lower[1] <= '7' && lower[2] == '\0') {
        *value = cpu_get_an(cpu, lower[1] - '0');
        return true;
    }

    return false;
}

// Parse an address string with optional L:/P: prefix and $/0x notation.
// Bare numbers without prefix are parsed as hexadecimal.
bool parse_address(const char *str, uint32_t *addr_out, addr_space_t *space_out) {
    if (!str || !addr_out || !space_out)
        return false;

    // Default to logical address
    *space_out = ADDR_LOGICAL;

    // Check for L: or P: prefix (case-insensitive)
    if ((str[0] == 'L' || str[0] == 'l') && str[1] == ':') {
        *space_out = ADDR_LOGICAL;
        str += 2;
    } else if ((str[0] == 'P' || str[0] == 'p') && str[1] == ':') {
        *space_out = ADDR_PHYSICAL;
        str += 2;
    }

    // Skip leading whitespace
    while (*str == ' ' || *str == '\t')
        str++;

    if (*str == '\0')
        return false;

    // Check for $ prefix — try register name first, then hex (IMP-407)
    if (*str == '$') {
        str++;
        // Try to resolve as register name (pc, sp, a0-a7, d0-d7, ssp, usp)
        if (resolve_register_address(str, addr_out))
            return true;
        // Fall through to hex parsing
        char *endptr;
        *addr_out = (uint32_t)strtoul(str, &endptr, 16);
        return *endptr == '\0';
    }

    // Check for 0x prefix (C hex)
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        char *endptr;
        *addr_out = (uint32_t)strtoul(str, &endptr, 0);
        return *endptr == '\0';
    }

    // Bare number — parse as hex by default
    char *endptr;
    *addr_out = (uint32_t)strtoul(str, &endptr, 16);
    return *endptr == '\0';
}

// Format a single address with $ prefix and uppercase hex: "$XXXXXXXX"
int format_address(char *buf, size_t buf_size, uint32_t addr) {
    return snprintf(buf, buf_size, "$%08X", addr);
}

// Check if dual address display should be active
bool addr_display_is_expanded(void) {
    switch (g_addr_display_mode) {
    case ADDR_DISPLAY_EXPANDED:
        return true;
    case ADDR_DISPLAY_COLLAPSED:
        return false;
    case ADDR_DISPLAY_AUTO:
    default:
        // Expanded when MMU is present and enabled
        return g_mmu && g_mmu->enabled;
    }
}

// Translate a logical address to physical for debug display.
uint32_t debug_translate_address(uint32_t logical_addr, bool *is_identity, bool *tt_hit, bool *valid) {
    if (is_identity)
        *is_identity = true;
    if (tt_hit)
        *tt_hit = false;
    if (valid)
        *valid = true;

    // No MMU or MMU disabled: identity mapping
    if (!g_mmu || !g_mmu->enabled)
        return logical_addr;

    // Check transparent translation first
    if (mmu_check_tt(g_mmu, logical_addr, false, true)) {
        if (tt_hit)
            *tt_hit = true;
        // TT = identity mapping
        return logical_addr;
    }

    // Perform table walk (read-only, supervisor mode for debug access)
    uint16_t mmusr = mmu_test_address(g_mmu, logical_addr, false, true, NULL);

    if (mmusr & MMUSR_I) {
        // Invalid descriptor
        if (valid)
            *valid = false;
        return logical_addr;
    }

    if (mmusr & MMUSR_B) {
        // Bus error during walk
        if (valid)
            *valid = false;
        return logical_addr;
    }

    // The MMUSR doesn't directly give us the physical address.
    // We need to do a full walk to get it.  Use mmu_table_walk result
    // indirectly by reading the physical_addr from a walk result.
    // For now, re-walk to extract physical address.
    // Note: mmu_table_walk is static in mmu.c, so we use mmu_translate_debug.
    uint32_t phys_addr = mmu_translate_debug(g_mmu, logical_addr);

    if (is_identity)
        *is_identity = (phys_addr == logical_addr);

    return phys_addr;
}

// Format an address with optional L:/P: dual display
int format_address_with_space(char *buf, size_t buf_size, uint32_t addr, addr_space_t space) {
    if (space == ADDR_PHYSICAL)
        return snprintf(buf, buf_size, "P:$%08X", addr);

    // Logical address — check if we should show dual L:/P:
    if (!addr_display_is_expanded())
        return snprintf(buf, buf_size, "$%08X", addr);

    // Expanded mode: show both L: and P:
    bool is_identity = true;
    bool valid = true;
    uint32_t phys_addr = debug_translate_address(addr, &is_identity, NULL, &valid);

    if (!valid)
        return snprintf(buf, buf_size, "L:$%08X P:????????", addr);

    return snprintf(buf, buf_size, "L:$%08X P:$%08X", addr, phys_addr);
}

// Format a logical address with optional physical translation for dual display
int format_address_pair(char *buf, size_t buf_size, uint32_t logical_addr) {
    return format_address_with_space(buf, buf_size, logical_addr, ADDR_LOGICAL);
}
