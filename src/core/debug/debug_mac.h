// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug_mac.h
// Mac-specific debugging interface: trap names, global variable lookup, and process inspection.

#ifndef DEBUG_MAC_H
#define DEBUG_MAC_H

#include <stdbool.h>
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

// === Public mouse / trace control ===========================================
//
// Backing entry points used by the typed `mouse.move` / `mouse.click` /
// `mouse.trace` root methods.

// Set mouse position with explicit routing mode. Mode chars:
//   'g' = global (Mac OS Toolbox MTemp + MTemp guard)
//   'h' = hardware (raw quadrature / ADB delta)
//   'a' = aux (A/UX MAE physical-page write)
//   else = default (per-platform best route)
// Returns 0 on success, -1 if the memory system isn't initialised.
int debug_mac_set_mouse_mode(long x, long y, char mode);

// Inject a mouse button event with explicit routing mode.
//   'g' = global (write MBState directly)
//   else = hw / default (route through ADB/VIA PB3)
void debug_mac_mouse_button_mode(bool button_down, char mode);

// Toggle the 1 Hz mouse-position trace logger. Equivalent to
// `trace-mouse start` / `trace-mouse stop`.
void debug_mac_set_trace_mouse(bool enabled);

// Resolve a key name (e.g. "return", "esc", "0x24") to an ADB
// keycode (0..0x7F). Returns -1 if the name doesn't match any
// registered alias and isn't a 0xNN hex literal in range.
int debug_mac_resolve_key_name(const char *name);

#endif // DEBUG_MAC_H
