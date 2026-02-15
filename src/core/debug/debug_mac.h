// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug_mac.h
// Mac-specific debugging interface: trap names, global variable lookup, and process inspection.

#ifndef DEBUG_MAC_H
#define DEBUG_MAC_H

#include <stdint.h>

// Returns the human-readable name for a Mac OS A-trap opcode
const char *macos_atrap_name(uint16_t trap);

// Initializes the Mac debug subsystem
void debug_mac_init(void);

// Prints process information (same as 'pi' debugger command)
void debug_mac_print_process_info(void);

// Looks up a global variable address by name, returns 0 if not found
uint32_t debug_mac_lookup_global_address(const char *name);

// Prints target 68K backtrace by walking stack frames
void debug_mac_print_target_backtrace(void);

// Prints current Mac application process info with header
void debug_mac_print_process_info_header(void);

#endif // DEBUG_MAC_H
