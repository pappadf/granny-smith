// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk.h
// Public interface for the AppleTalk networking protocol stack and the AFP
// file server that rides on it.
//
// The object-model surface built on top of this API is
// `appletalk` / `appletalk.afp` / `appletalk.printer`
// (proposal-appletalk-afp-object-model.md §2).  Every call that can fail for
// a reason a user should see reports it through an `err`/`err_len` buffer, so
// the tree can surface the real message instead of "failed (see log)".

#ifndef APPLETALK_H
#define APPLETALK_H

// === Includes ===
#include "common.h"
#include "platform.h"

// === Forward Declarations ===
typedef struct scc scc_t;
typedef struct scheduler scheduler_t;

// === Stack-level state (object model: `appletalk`) ==========================

// Attach/detach the stack from the SCC link.  Enabled by default; disabling
// makes the emulated machine behave as if nothing were on the network.
bool atalk_get_enabled(void);
void atalk_set_enabled(bool enabled);

// Current LLAP node ID (0 while unassigned).
unsigned atalk_node_id(void);

// Link- and transport-level counters (object model: `appletalk.stats`).
typedef struct {
    uint64_t llap_rx;
    uint64_t llap_tx;
    uint64_t malformed; // frames discarded as malformed, at any layer
    uint64_t unhandled; // well-formed frames nothing here serves
    uint64_t tx_dropped; // frames the stack gave up transmitting
    uint64_t ddp_in;
    uint64_t ddp_out;
    uint64_t atp_requests;
    uint64_t atp_retries;
    uint64_t nbp_packets;
} atalk_stats_t;

const atalk_stats_t *atalk_get_stats(void);

// === NBP registry views (object model: `appletalk.nbp`) =====================

typedef struct {
    char object[33];
    char type[33];
    char zone[33];
    unsigned socket;
    unsigned node;
    unsigned net;
} atalk_nbp_info_t;

int atalk_nbp_entry_max(void); // upper bound on the entry index
bool atalk_nbp_entry_in_use(int index);
bool atalk_nbp_entry_info(int index, atalk_nbp_info_t *out);

// === AFP volumes (object model: `appletalk.afp.volumes`) ====================
//
// A volume is a host directory published under an AFP name.  Slots are stable
// for the volume's lifetime; empty slots are holes in the collection.

// Publish `path` as the AFP volume `name`.  Returns the slot, or -1 with the
// reason written into `err`.
int atalk_afp_volume_add(const char *name, const char *path, char *err, size_t err_len);

// The same, keeping the volume id a checkpoint recorded, so a restored guest's
// cached id still names it.  Fails if the id is taken.
int atalk_afp_volume_restore(const char *name, const char *path, unsigned vol_id, char *err, size_t err_len);

// Withdraw a volume by name, closing its forks and flushing its catalog.
int atalk_afp_volume_remove(const char *name, char *err, size_t err_len);

int atalk_afp_volume_max(void);
int atalk_afp_volume_find(const char *name); // slot, or -1
bool atalk_afp_volume_in_use(int slot);
const char *atalk_afp_volume_name(int slot);
const char *atalk_afp_volume_path(int slot);
unsigned atalk_afp_volume_vol_id(int slot);
unsigned atalk_afp_volume_open_forks(int slot);
unsigned atalk_afp_volume_sessions_using(int slot);
unsigned atalk_afp_volume_catalog_generation(int slot);
unsigned atalk_afp_volume_cnid_count(int slot);

// === AFP server identity (object model: `appletalk.afp`) ====================

const char *atalk_afp_get_name(void);
int atalk_afp_set_name(const char *name, char *err, size_t err_len);

// Serving and advertising.  Turning the server off deregisters its NBP entry,
// warns and drops live sessions, and refuses new ones.
bool atalk_afp_get_enabled(void);
int atalk_afp_set_enabled(bool enabled, char *err, size_t err_len);

// The server message clients fetch with FPGetSrvrMsg.  Setting a non-empty
// message raises the ASP "server message available" attention.
const char *atalk_afp_get_message(void);
int atalk_afp_set_message(const char *message, char *err, size_t err_len);

// The AFP version strings we advertise and accept at login.
const char *const *atalk_afp_versions(int *count);

// Server-wide counters (object model: `appletalk.afp.stats`).
typedef struct {
    uint64_t commands_served;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t errors;
    uint64_t open_forks;
} atalk_afp_stats_t;

const atalk_afp_stats_t *atalk_afp_get_stats(void);

// Per-result-code error tally.  `atalk_afp_error_code_at` walks the codes that
// have actually occurred, so the map the object model publishes stays small.
uint64_t atalk_afp_error_count(int32_t code);
int atalk_afp_error_code_at(int index, int32_t *out_code, uint64_t *out_count);

// === ASP sessions (object model: `appletalk.afp.sessions`) ==================

typedef struct {
    unsigned session_ref;
    unsigned client_node;
    unsigned socket;
    char afp_version[24];
    unsigned open_forks;
    uint64_t idle_ns; // since the last packet from this client
} atalk_session_info_t;

int atalk_asp_session_max(void);
bool atalk_asp_session_in_use(int index);
bool atalk_asp_session_info(int index, atalk_session_info_t *out);

// ASP Attention codes we raise (AFP_21_22 Table 1-7).
#define ATALK_ATTN_SHUTDOWN   0x8000u // shutdown, no message
#define ATALK_ATTN_SERVER_MSG 0x2000u // a server message is available

// Send an ASP Attention to one session / to every open session.
int atalk_asp_send_attention(uint16_t session_ref, uint16_t code);
void atalk_asp_broadcast_attention(uint16_t code);

// Tear down every ASP session (server disable, machine teardown).
void atalk_asp_close_all_sessions(void);

// === Printer (object model: `appletalk.printer`) ============================

bool atalk_printer_is_enabled(void);
const char *atalk_printer_object_name(void);
int atalk_printer_set_enabled(bool enabled, char *err, size_t err_len);
int atalk_printer_set_name(const char *name, char *err, size_t err_len);

// The PAP status string as the workstation reads it.
const char *atalk_printer_status_text(void);

// True when the build links the PostScript interpreter (PLATEN=1); then a
// job produces a PDF through the platform sink and the capture is optional.
bool atalk_printer_has_interpreter(void);

// Whether each job's PostScript is also handed to the platform, as it was
// sent (laserwriter_sink_capture): a .ps beside the PDF, or a download.
bool atalk_printer_capture_get(void);
void atalk_printer_capture_set(bool enabled);

// Printer counters (object model: `appletalk.printer.stats`) -- what a
// script needs to see that a job happened, how big it was and what became of
// it; the printer published two attributes and nothing else (10-network F-33).
typedef struct {
    uint64_t jobs; // jobs that ran to their end
    uint64_t aborts; // jobs cut off: timeout, too large, closed early
    uint64_t bytes; // PostScript bytes received
    uint64_t captures; // captures handed to the platform
    uint64_t last_capture; // bytes in the last of them
} atalk_printer_stats_t;

const atalk_printer_stats_t *atalk_printer_get_stats(void);

// Documents produced, and the page count and outcome of the last finished
// job ("" before any; "ok"; "error: <name> in <command>"; "budget").
uint32_t atalk_printer_documents(void);
uint32_t atalk_printer_last_pages(void);
const char *atalk_printer_last_outcome(void);

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

// Advertise `desc` through `*entry`: registered when *entry is NULL, updated in
// place otherwise.  All or nothing: on failure -- the name is taken, the table
// is full -- *entry and what it advertises are unchanged, so a service that
// publishes a new name before storing it keeps the old one whole.  Renames
// used to store the new name first (AFP, the printer: the tree showed a name
// nobody could look up) or withdraw first (PPC: the port vanished) --
// 10-network N-23.  0, or -1 with nothing changed.
int atalk_nbp_publish(atalk_nbp_entry_t **entry, const atalk_nbp_service_desc_t *desc);

// Withdraw *entry, if published, and clear it.
void atalk_nbp_withdraw(atalk_nbp_entry_t **entry);

// Look an entity pattern up on the network.  `object` and `type` may use the
// NBP wildcards ("=" matches everything); replies are delivered to `cb` one
// tuple at a time as they arrive, so the caller must run the scheduler before
// expecting results.  One lookup is outstanding at a time: issuing another
// replaces it.  Returns 0 if the request went out.
typedef void (*atalk_nbp_reply_fn)(void *ctx, const atalk_nbp_info_t *info);
int atalk_nbp_lookup(const char *object, const char *type, const char *zone, uint8_t reply_socket,
                     atalk_nbp_reply_fn cb, void *ctx);
void atalk_nbp_lookup_cancel(void);

// === ASP Status Block ===

// === Lifecycle (Constructor / Destructor) ===

// Initialization hook for AppleTalk module (registers shell commands)
void appletalk_init(scheduler_t *scheduler, scc_t *scc, checkpoint_t *checkpoint);

// Serialize the AppleTalk/AFP session and fork tables into a checkpoint.
void appletalk_checkpoint(checkpoint_t *checkpoint);

// Destructor: `scc` is the departing machine's SCC.  A no-op unless the stack
// is bound to that machine (see appletalk.c).
void appletalk_delete(scc_t *scc);

// Server module hooks: publish the NBP advertisement at startup, release
// volumes and forks at teardown.
void atalk_server_init(void);
void atalk_server_delete(void);

#endif // APPLETALK_H
