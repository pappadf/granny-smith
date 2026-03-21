// SPDX-License-Identifier: MIT
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
    ADDR_LOGICAL, // default — address as seen by CPU (MMU-translated)
    ADDR_PHYSICAL // physical bus address (bypass MMU)
} addr_space_t;

// Address display mode (controls when dual L:/P: display is shown)
typedef enum {
    ADDR_DISPLAY_AUTO, // collapsed when MMU off; expanded when MMU on
    ADDR_DISPLAY_COLLAPSED, // always single address unless L != P
    ADDR_DISPLAY_EXPANDED // always show both L: and P:
} addr_display_mode_t;

// Global display mode, settable via "addrmode" shell command
extern addr_display_mode_t g_addr_display_mode;

// Parse an address string with optional L:/P: prefix and $/0x notation.
// Returns true on success, fills out addr and space.
// Handles: "$408000", "0x408000", "L:$408000", "P:0x408000", "408000"
// Bare numbers without prefix are parsed as hexadecimal.
bool parse_address(const char *str, uint32_t *addr_out, addr_space_t *space_out);

// Format a single address with $ prefix and uppercase hex.
// Writes "$XXXXXXXX" into buf.  Returns number of characters written.
int format_address(char *buf, size_t buf_size, uint32_t addr);

// Format an address with optional L:/P: dual display.
// If space is ADDR_PHYSICAL, always shows "P:$XXXXXXXX".
// If space is ADDR_LOGICAL, may show "L:$XXX P:$XXX" depending on display mode
// and MMU state.  Returns number of characters written.
int format_address_with_space(char *buf, size_t buf_size, uint32_t addr, addr_space_t space);

// Format a logical address with optional physical translation for dual display.
// Checks current display mode and MMU state.
// Used by disasm, prompt, examine, etc.
int format_address_pair(char *buf, size_t buf_size, uint32_t logical_addr);

// Translate a logical address to physical for debug display.
// Returns the physical address.  If no MMU or MMU disabled, returns logical_addr.
// Sets *is_identity to true if logical == physical.
// Sets *tt_hit to true if the address matched a transparent translation register.
// Sets *valid to true if translation succeeded.
uint32_t debug_translate_address(uint32_t logical_addr, bool *is_identity, bool *tt_hit, bool *valid);

// Check if dual address display should be shown (based on display mode and MMU state).
bool addr_display_is_expanded(void);

#endif // ADDR_FORMAT_H
