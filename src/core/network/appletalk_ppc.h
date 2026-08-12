// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_ppc.h
// The PPC Toolbox session layer: a host program-linking port advertised over
// NBP, guest-mode sessions in both directions over ADSP, and the port browse
// that finds what a guest has to offer.
//
// Coding reference: docs/core/network/ppc_appleevents.md §2 (record layouts),
// §3 (discovery), §4 (the session layer).  Its only client is the Apple event
// layer in appletalk_aevt.c.

#ifndef APPLETALK_PPC_H
#define APPLETALK_PPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Forward declarations ===
struct object;
typedef struct ppc_session ppc_session_t;

// === Constants (ppc_appleevents.md §2, §4) ==================================

#define PPC_NBP_TYPE      "PPCToolBox"
#define PPC_HOST_SOCKET   130 // our connection-listening socket
#define PPC_CLIENT_SOCKET 131 // the socket our outgoing connections use

#define PPC_PORT_REC_SIZE        72
#define PPC_LOCATION_SIZE        104
#define PPC_PORT_INFO_SIZE       74
#define PPC_SESSION_REQUEST_SIZE 290
#define PPC_ANSWER_SIZE          8
#define PPC_LIST_REQUEST_SIZE    114
#define PPC_LIST_TRAILER_SIZE    6
#define PPC_BLOCK_HEADER_SIZE    12

#define PPC_MAX_SESSIONS 8
#define PPC_MAX_PORTS    32
#define PPC_MAX_MESSAGE  32768 // largest reassembled message block we accept

// Rejection reasons (§4.2).
#define PPC_REJECT_PORT_NOT_SHARED 1
#define PPC_REJECT_UNKNOWN_PORT    2
#define PPC_REJECT_UNKNOWN_USER    3
#define PPC_REJECT_BAD_PASSWORD    4
#define PPC_REJECT_NOT_LISTENING   5
#define PPC_REJECT_GUESTS_REFUSED  6
#define PPC_REJECT_LINKING_OFF     7

typedef enum {
    PPC_SESSION_FREE = 0,
    PPC_SESSION_CONNECTING, // ADSP open dialog in flight
    PPC_SESSION_REQUESTED, // session request written, awaiting the answer
    PPC_SESSION_OPEN, // message blocks may flow
    PPC_SESSION_FAILED,
} ppc_session_state_t;

extern const char *const PPC_SESSION_STATE_NAMES[];
#define PPC_SESSION_STATE_COUNT 5

// One port discovered on the network (§4.6).
typedef struct {
    char name[33];
    char type[33];
    char machine[33]; // the NBP object name of the machine holding it
    uint8_t node;
    uint8_t socket;
    bool auth_required;
} ppc_port_info_t;

// === Session client interface ===============================================

typedef struct {
    // The far side accepted: message blocks may now be written.
    void (*on_ready)(void *ctx, ppc_session_t *s);
    // The session will not happen, or has ended badly.  `reason` is a phrase.
    void (*on_failed)(void *ctx, ppc_session_t *s, const char *reason);
    // A complete message block arrived (§4.7).
    void (*on_block)(void *ctx, ppc_session_t *s, uint32_t creator, uint32_t type, uint32_t user_data,
                     const uint8_t *payload, int len);
    void (*on_closed)(void *ctx, ppc_session_t *s);
} ppc_client_t;

// === Lifecycle ==============================================================

void atalk_ppc_init(void);
void atalk_ppc_shutdown(void);

// Publish (or withdraw) the host port.  Registering it puts the NBP entity on
// the network and starts accepting guest sessions on PPC_HOST_SOCKET.
int atalk_ppc_set_host_port(const char *name, bool enabled, char *err, size_t err_len);
const char *atalk_ppc_host_port_name(void);

// Where inbound events go.  Set once by the Apple event layer.
void atalk_ppc_set_inbound_client(const ppc_client_t *client, void *ctx);

// === Browse (§3, §4.6) ======================================================

// Kick off a fresh discovery: NBP lookup for PPC machines, then a list-ports
// exchange with each.  Non-blocking — results land in the port table as the
// guest answers, so the caller runs the scheduler and reads it afterwards.
int atalk_ppc_browse(char *err, size_t err_len);

int atalk_ppc_port_count(void);
bool atalk_ppc_port_info(int index, ppc_port_info_t *out);
int atalk_ppc_port_find(const char *name); // index, or -1
bool atalk_ppc_browse_in_flight(void);

// === Sessions ===============================================================

// Open a guest session to a discovered port.  Returns NULL with a reason in
// `err` when the port is unknown or no session slot is free.  The session is
// not usable until `on_ready` fires.
ppc_session_t *atalk_ppc_open(const char *port_name, const ppc_client_t *client, void *ctx, char *err, size_t err_len);

// An already-open session to that port, or NULL.
ppc_session_t *atalk_ppc_find_open(const char *port_name);

// Write one message block (§4.7).  Returns 0 on success.
int atalk_ppc_send_block(ppc_session_t *s, uint32_t creator, uint32_t type, uint32_t user_data, const uint8_t *payload,
                         int len);

void atalk_ppc_close(ppc_session_t *s, const char *reason);
void atalk_ppc_close_all(const char *reason);

// Accessors for the object model.
int atalk_ppc_session_slot_max(void);
ppc_session_t *atalk_ppc_session_at(int slot);
ppc_session_state_t atalk_ppc_session_state(const ppc_session_t *s);
bool atalk_ppc_session_initiator(const ppc_session_t *s);
const char *atalk_ppc_session_port(const ppc_session_t *s);
uint8_t atalk_ppc_session_peer_node(const ppc_session_t *s);
uint64_t atalk_ppc_session_bytes_in(const ppc_session_t *s);
uint64_t atalk_ppc_session_bytes_out(const ppc_session_t *s);
uint32_t atalk_ppc_session_id(const ppc_session_t *s);

// === Counters ===============================================================

typedef struct {
    uint64_t sessions_opened;
    uint64_t sessions_rejected;
    uint64_t sessions_refused; // rejections we sent
    uint64_t blocks_in;
    uint64_t blocks_out;
    uint64_t browses;
} ppc_stats_t;

const ppc_stats_t *atalk_ppc_get_stats(void);

// === Object model ===========================================================

void atalk_ppc_install_objects(struct object *parent);
void atalk_ppc_remove_objects(void);

#endif // APPLETALK_PPC_H
