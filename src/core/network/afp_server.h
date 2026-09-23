// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_server.h
// The AFP server's entry points below the object model: what ASP reaches
// through the client the server registers (appletalk_asp.h), exported so the
// afp unit suite can drive the server with no transport at all.  Nothing in
// the stack calls these by name.

#ifndef AFP_SERVER_H
#define AFP_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One AFP command from session `session_id`.  Returns the AFP result code.
uint32_t afp_handle_command(uint16_t session_id, uint8_t opcode, const uint8_t *in, int in_len, uint8_t *out,
                            int out_max, int *out_len);

// A session opened (ASP's OpenSess): make its AFP record, not yet logged in.
// False refuses it -- the server is disabled, or its table is full.
bool afp_session_opened(uint16_t session_ref);

// Release the forks, enumeration snapshots and volume references a departing
// session held, and drop its record.
void afp_session_closed(uint16_t session_id);

// The AFP version a session logged in with ("" before FPLogin), or NULL.
const char *afp_session_version(uint16_t session_ref);

// Drop every reconstructible per-session cache.
void afp_reset_transient_state(void);

// Forks a session currently holds open.
uint32_t afp_session_open_forks(uint16_t session_id);

// Build the ASP GetStatus Service Status Block (per docs/errata.md layout).
// Inputs: server_name and machine_type as C-strings (may be NULL -> treated as empty).
// Contents: the AFP version list the server actually implements and the UAM
//           list ["No User Authent"].  No icon/mask is included (offset=0).
// Output: *out_buf points to malloc'd buffer and *out_len is its size. Caller must free(*out_buf).
// Returns 0 on success, non-zero on failure (e.g., allocation failure).
int atalk_build_status_block(const char *server_name, const char *machine_type, uint8_t **out_buf, size_t *out_len);

#endif // AFP_SERVER_H
