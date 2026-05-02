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

// === M8 — public mouse / trace control =====================================
//
// Thin entry points around the file-private helpers used by the legacy
// `set-mouse` / `mouse-button` / `trace-mouse` commands. The
// `input.mouse` object class calls these so both the legacy shell
// path and the new tree path share the same backing logic.

// Set mouse position via the per-platform default route (Mac OS uses
// global write; A/UX MAE uses MAE-resolved physical write; others
// fall back to hardware delta injection). Equivalent to the
// no-flag `set-mouse x y` shell form.
void debug_mac_set_mouse(long x, long y);

// Toggle the 1 Hz mouse-position trace logger. Equivalent to
// `trace-mouse start` / `trace-mouse stop`.
void debug_mac_set_trace_mouse(bool enabled);

#endif // DEBUG_MAC_H
