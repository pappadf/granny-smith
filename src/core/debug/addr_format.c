// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// addr_format.c
// Unified address parsing and formatting for logical/physical addresses.

#include "addr_format.h"

#include "debug_cpu.h" // register names, MMU state, translation

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Shown in place of the physical half's hex digits when a logical address
// does not translate: one '?' per digit of the "%08X" field it replaces
#define ADDR_UNMAPPED_TEXT "????????"

// Display mode: auto (collapsed unless the MMU is active)
static addr_display_mode_t s_addr_display_mode = ADDR_DISPLAY_AUTO;

addr_display_mode_t addr_display_get_mode(void) {
    return s_addr_display_mode;
}

void addr_display_set_mode(addr_display_mode_t mode) {
    s_addr_display_mode = mode;
}

// Parse an address string with optional L:/P: prefix and $/0x notation.
// Bare numbers without prefix are parsed as hexadecimal.
bool parse_address(const char *str, uint32_t *addr_out, addr_space_t *space_out) {
    if (!str || !addr_out || !space_out)
        return false;

    // Default to logical address
    *space_out = ADDR_SPACE_LOGICAL;

    // Check for L: or P: prefix (case-insensitive)
    if ((str[0] == 'L' || str[0] == 'l') && str[1] == ':') {
        *space_out = ADDR_SPACE_LOGICAL;
        str += 2;
    } else if ((str[0] == 'P' || str[0] == 'p') && str[1] == ':') {
        *space_out = ADDR_SPACE_PHYSICAL;
        str += 2;
    }

    // Skip leading whitespace
    while (*str == ' ' || *str == '\t')
        str++;

    if (*str == '\0')
        return false;

    // Check for $ prefix — try register name first, then hex.
    if (*str == '$') {
        str++;
        if (*str == '\0')
            return false; // explicit reject empty after $
        // Try to resolve as register name (pc, sp, a0-a7, d0-d7, ssp, usp)
        if (debug_cpu_register_value(str, addr_out))
            return true;
        // Fall through to hex parsing.
        char *endptr;
        errno = 0;
        unsigned long v = strtoul(str, &endptr, 16);
        if (errno || v > UINT32_MAX || *endptr != '\0')
            return false;
        *addr_out = (uint32_t)v;
        return true;
    }

    // Check for 0x prefix (C hex). Parse with explicit base 16 to avoid the
    // base-0 octal-on-leading-zero footgun (`017` parses differently otherwise).
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        char *endptr;
        errno = 0;
        unsigned long v = strtoul(str + 2, &endptr, 16);
        if (errno || v > UINT32_MAX || *endptr != '\0')
            return false;
        *addr_out = (uint32_t)v;
        return true;
    }

    // Bare number — parse as hex by default.
    char *endptr;
    errno = 0;
    unsigned long v = strtoul(str, &endptr, 16);
    if (errno || v > UINT32_MAX || *endptr != '\0')
        return false;
    *addr_out = (uint32_t)v;
    return true;
}

// Format a single address with $ prefix and uppercase hex: "$XXXXXXXX"
int format_address(char *buf, size_t buf_size, uint32_t addr) {
    return snprintf(buf, buf_size, "$%08X", addr);
}

// Check if dual address display should be active
bool addr_display_is_expanded(void) {
    switch (s_addr_display_mode) {
    case ADDR_DISPLAY_EXPANDED:
        return true;
    case ADDR_DISPLAY_COLLAPSED:
        return false;
    case ADDR_DISPLAY_AUTO:
    default:
        // Expanded when MMU is present and enabled
        return debug_cpu_mmu_enabled();
    }
}

// Format an address with optional L:/P: dual display
int format_address_with_space(char *buf, size_t buf_size, uint32_t addr, addr_space_t space) {
    if (space == ADDR_SPACE_PHYSICAL)
        return snprintf(buf, buf_size, "P:$%08X", addr);

    // Logical address — check if we should show dual L:/P:
    if (!addr_display_is_expanded())
        return snprintf(buf, buf_size, "$%08X", addr);

    // Expanded mode: show both L: and P:
    bool is_identity = true;
    bool valid = true;
    uint32_t phys_addr = debug_translate_address(addr, &is_identity, NULL, &valid);

    if (!valid)
        return snprintf(buf, buf_size, "L:$%08X P:" ADDR_UNMAPPED_TEXT, addr);

    return snprintf(buf, buf_size, "L:$%08X P:$%08X", addr, phys_addr);
}

// Format a logical address with optional physical translation for dual display
int format_address_pair(char *buf, size_t buf_size, uint32_t logical_addr) {
    return format_address_with_space(buf, buf_size, logical_addr, ADDR_SPACE_LOGICAL);
}
