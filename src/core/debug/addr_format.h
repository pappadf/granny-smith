// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// addr_format.h
// Unified address parsing and formatting for logical/physical addresses.
// Provides Motorola-convention $ hex prefix and L:/P: address space qualifiers.

#ifndef ADDR_FORMAT_H
#define ADDR_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Address space qualifier
typedef enum {
    ADDR_SPACE_LOGICAL, // default — address as seen by CPU (MMU-translated)
    ADDR_SPACE_PHYSICAL // physical bus address (bypass MMU)
} addr_space_t;

// Address display mode (controls when dual L:/P: display is shown)
typedef enum {
    ADDR_DISPLAY_AUTO, // collapsed when MMU off; expanded when MMU on
    ADDR_DISPLAY_COLLAPSED, // always single address unless L != P
    ADDR_DISPLAY_EXPANDED // always show both L: and P:
} addr_display_mode_t;

// Current display mode (ADDR_DISPLAY_AUTO until set)
addr_display_mode_t addr_display_get_mode(void);
void addr_display_set_mode(addr_display_mode_t mode);

// Parse an address string with optional L:/P: prefix and $/0x notation.
// Returns true on success, fills out addr and space.
// Handles: "$408000", "0x408000", "L:$408000", "P:0x408000", "408000", and
// "$pc"/"$d0"-style 68K register names (debug_cpu_register_value).
// Bare numbers without prefix are parsed as hexadecimal.
bool parse_address(const char *str, uint32_t *addr_out, addr_space_t *space_out);

// Format a single address with $ prefix and uppercase hex.
// Writes "$XXXXXXXX" into buf.  Returns number of characters written.
int format_address(char *buf, size_t buf_size, uint32_t addr);

// Format an address with optional L:/P: dual display.
// If space is ADDR_SPACE_PHYSICAL, always shows "P:$XXXXXXXX".
// If space is ADDR_SPACE_LOGICAL, may show "L:$XXX P:$XXX" depending on display mode
// and MMU state.  Returns number of characters written.
int format_address_with_space(char *buf, size_t buf_size, uint32_t addr, addr_space_t space);

// Format a logical address with optional physical translation for dual display.
// Checks current display mode and MMU state.
// Used by disasm, prompt, examine, etc.
int format_address_pair(char *buf, size_t buf_size, uint32_t logical_addr);

// Check if dual address display should be shown (based on display mode and MMU state).
bool addr_display_is_expanded(void);

#endif // ADDR_FORMAT_H
