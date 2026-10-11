// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug_mac.h
// Mac-specific debugging interface: trap names, global variable lookup, and process inspection.

#ifndef DEBUG_MAC_H
#define DEBUG_MAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Returns the name of a Mac OS A-trap opcode ("_GetResource"), or "_XXXX"
// hex for an unknown one.  A known name points into the static table; an
// unknown one is written into `buf` (8 bytes suffice) and `buf` returned.
// Defined in mac_traps_data.c, beside its table.
const char *debug_mac_atrap_name(uint16_t trap, char *buf, size_t buf_size);

// Initializes the Mac debug subsystem

// Prints process information (same as 'pi' debugger command)
void debug_mac_print_process_info(void);

// Looks up a global variable address by name, returns 0 if not found
uint32_t debug_mac_lookup_global_address(const char *name);

// Reverse-lookup: given an address, return the global's name (or NULL).
// Used by the `re` disassembler to annotate absolute-address operands in
// the low-memory globals range.  Returns a pointer to the static name
// string (no caller-side free).
const char *debug_mac_lookup_global_name(uint32_t address);

// Prints target 68K backtrace by walking stack frames
void debug_mac_print_target_backtrace(void);

// Prints current Mac application process info with header
void debug_mac_print_process_info_header(void);

// === Public mouse / trace control ===========================================
//
// Backing entry points used by the typed `mouse.move` / `mouse.click` /
// `mouse.trace` root methods.  The trace is a scheduler event whose source
// is the machine's host_input object (host_input.h), which registers the
// callback below at construction.

struct host_input;

// The trace's 1 Hz sample.  A scheduler event callback; the payload is its
// whole state.
void debug_mac_mouse_trace_tick(void *source, uint64_t data);

// How a mouse.move / mouse.click reaches the guest (the `mode` argument,
// parsed by input_mouse_mode_parse in mouse_class.c).
typedef enum mouse_route {
    MOUSE_ROUTE_INVALID = 0, // not a mode (the parser's "bad argument")
    MOUSE_ROUTE_DEFAULT, // per-platform best route
    MOUSE_ROUTE_GLOBAL, // Mac OS Toolbox low-memory globals (MTemp, MBState)
    MOUSE_ROUTE_HW, // hardware: raw quadrature / ADB delta, VIA PB3 / ADB button
    MOUSE_ROUTE_AUX, // A/UX MAE: physical-page writes through the cached user CRP
} mouse_route_t;

// Set mouse position with explicit routing mode.
// Returns 0 on success, -1 if the memory system isn't initialised or the
// route could not deliver the position (the reason is printed).
int debug_mac_set_mouse_mode(long x, long y, mouse_route_t route);

// Inject a mouse button event with explicit routing mode.  GLOBAL writes
// MBState directly; every other route (AUX included: the button has no
// per-process copy to target) goes through the ADB/VIA PB3 hardware path.
void debug_mac_mouse_button_mode(bool button_down, mouse_route_t route);

// Toggle the 1 Hz mouse-position trace logger on the machine `hi` belongs to.
void debug_mac_set_trace_mouse(struct host_input *hi, bool enabled);

// Resolve a key name (e.g. "return", "esc", "0x24") to an ADB
// keycode (0..0x7F). Returns -1 if the name doesn't match any
// registered alias and isn't a 0xNN hex literal in range.
int debug_mac_resolve_key_name(const char *name);

// Resolve one printable ASCII character to the US-layout ADB keycode that
// produces it; *shift says whether Shift must be held.  -1 = untypable.
int debug_mac_resolve_ascii(char c, bool *shift);

#endif // DEBUG_MAC_H
