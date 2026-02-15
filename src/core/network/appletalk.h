// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk.h
// Public interface for AppleTalk networking protocol stack.

#ifndef APPLETALK_H
#define APPLETALK_H

// === Includes ===
#include "common.h"
#include "platform.h"

// === Forward Declarations ===
typedef struct scc scc_t;
typedef struct scheduler scheduler_t;

// === Operations ===

// Entry point from SCC SDLC to feed a LocalTalk frame (LLAP) to AppleTalk stack
void process_packet(uint8_t *buf, size_t size);

// AppleTalk share management (AppleShare/AFP server) API exposed for shell cmds
// Name: up to 32 chars; Path: POSIX path inside Emscripten MEMFS
int atalk_share_add(const char *name, const char *path);

int atalk_share_remove(const char *name);

int atalk_share_list(void);

const char *atalk_server_object_name(void);

// === NBP (Name Binding Protocol) ===

// NBP service publication helpers.
// Server/printer modules use these to expose NVEs without touching the core NBP tables.
typedef struct atalk_nbp_entry atalk_nbp_entry_t; // opaque handle returned on registration

typedef struct {
    const char *object; // required, max 32 chars
    const char *type; // required, max 32 chars
    const char *zone; // optional, defaults to "*"
    uint8_t socket; // required destination socket
    uint8_t node; // optional, defaults to LLAP_HOST_NODE
    uint16_t net; // optional, defaults to 0 (unknown)
} atalk_nbp_service_desc_t;

int atalk_nbp_register(const atalk_nbp_service_desc_t *desc, atalk_nbp_entry_t **out_entry);

int atalk_nbp_update(atalk_nbp_entry_t *entry, const atalk_nbp_service_desc_t *desc);

int atalk_nbp_unregister(atalk_nbp_entry_t *entry);

// === Printer Control ===

int atalk_printer_enable(const char *object_name); // enable (or rename) LaserWriter entity

int atalk_printer_disable(void); // disable LaserWriter advertisement

// === ASP Status Block ===

// Build the ASP GetStatus Service Status Block (per docs/errata.md layout).
// Inputs: server_name and machine_type as C-strings (may be NULL → treated as empty).
// Contents: hard-coded AFP versions ["AFPVersion 2.0", "AFPVersion 2.1"],
//           UAM list ["No User Authent"]. No icon/mask is included (offset=0).
// Output: *out_buf points to malloc'd buffer and *out_len is its size. Caller must free(*out_buf).
// Returns 0 on success, non-zero on failure (e.g., allocation failure).
int atalk_build_status_block(const char *server_name, const char *machine_type, uint8_t **out_buf, size_t *out_len);

// === Lifecycle (Constructor / Destructor) ===

// Initialization hook for AppleTalk module (registers shell commands)
void appletalk_init(scheduler_t *scheduler, scc_t *scc, checkpoint_t *checkpoint);

// Destructor (no-op for now)
void appletalk_delete(void);

// Server module hook so it can publish its NBP advertisement at startup
void atalk_server_init(void);

#endif // APPLETALK_H
