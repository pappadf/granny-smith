// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_ppc.c
// PPC Toolbox session layer over ADSP.
//
// Coding reference: docs/internals/core/network/ppc_appleevents.md — §2 for the record
// layouts, §3 for NBP discovery, §4 for the session dialog and message-block
// framing.  Section numbers in the comments refer to that document.
//
// Everything a guest sends is untrusted: block sizes are checked before use,
// reassembly is bounded, and a message we cannot parse ends the session with
// a readable reason instead of corrupting state.

// ============================================================================
// Includes
// ============================================================================

#include "appletalk_ppc.h"

#include "appletalk.h"
#include "appletalk_adsp.h"
#include "appletalk_internal.h"
#include "common.h"
#include "log.h"
#include "object.h"
#include "value.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("ppctoolbox");

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

// ============================================================================
// Constants and Macros
// ============================================================================

// Session message type codes (§4.2), as 32-bit big-endian values.
#define PPC_MSG_SREQ 0x53524551u
#define PPC_MSG_SAPT 0x53415054u
#define PPC_MSG_SREJ 0x5352454Au
#define PPC_MSG_UREJ 0x5552454Au
#define PPC_MSG_ACNT 0x41434E54u
#define PPC_MSG_ARSP 0x41525350u
#define PPC_MSG_LPRT 0x4C505254u
#define PPC_MSG_LRSP 0x4C525350u

#define PPC_KIND_BY_STRING 2 // portKindSelector (§2.1)
#define PPC_LOC_NBP        1 // locationKindSelector (§2.2)
#define PPC_LIST_MAX_BATCH 7 // PortInfoRecs per write (§4.6)

// How many ports we ask a machine for in one browse.
#define PPC_LIST_REQUEST_COUNT 32

const char *const PPC_SESSION_STATE_NAMES[] = {"free", "connecting", "requested", "open", "failed", NULL};

// ============================================================================
// Type Definitions
// ============================================================================

// What a session is being used for.  A browse borrows the same machinery as a
// real session but talks the list-ports dialog instead (§4.6).
typedef enum {
    PPC_USE_SESSION = 0,
    PPC_USE_BROWSE,
} ppc_use_t;

struct ppc_session {
    bool in_use;
    int slot;
    uint32_t id;
    ppc_session_state_t state;
    ppc_use_t use;
    bool initiator;

    adsp_conn_t *conn;
    char port_name[33];
    char machine[33];
    uint8_t peer_node;
    uint8_t peer_socket;
    uint8_t local_socket; // ours, unique per outgoing connection

    const ppc_client_t *client;
    void *client_ctx;

    // Reassembly of the current inbound ADSP message (§4.1): bytes accumulate
    // until the end-of-message marker.
    uint8_t *rx;
    int rx_len;

    // Browse bookkeeping.
    int ports_collected;

    uint64_t bytes_in;
    uint64_t bytes_out;
};

// ============================================================================
// Module state
// ============================================================================

// PPC's part of the network: the host port -- the network's program-linking
// peer, published by NBP -- where its inbound sessions go, and the
// `appletalk.ppc` collections' entries.  atalk_ppc_init makes it; the network
// owns it.
struct ppc_host {
    char port[33];
    bool enabled;
    atalk_nbp_entry_t *nbp;
    const ppc_client_t *inbound_client;
    void *inbound_ctx;
    object_cache_t port_entries;
    object_cache_t session_entries;
};

// The network's, set by atalk_ppc_init.
static ppc_host_t *g_host;

// Counters published as `appletalk.ppc.stats`.
typedef struct {
    uint64_t sessions_opened;
    uint64_t sessions_rejected;
    uint64_t sessions_refused; // rejections we sent
    uint64_t blocks_in;
    uint64_t blocks_out;
    uint64_t browses;
    uint64_t malformed; // message blocks too short for their header
} ppc_stats_t;

// Machines the current browse has found but not yet queried.
typedef struct {
    char name[33];
    uint8_t node;
    uint8_t socket;
} ppc_machine_t;

#define PPC_MAX_MACHINES 8

// PPC's part of a machine's connection (atalk_conn_t): the sessions with that
// Mac, what a browse of it has found, and the traffic's counters.
struct ppc_link {
    ppc_stats_t stats; // published as `appletalk.ppc.stats`
    ppc_session_t sessions[PPC_MAX_SESSIONS];
    uint32_t next_session_id;
    ppc_port_info_t ports[PPC_MAX_PORTS];
    int port_count;
    ppc_machine_t machines[PPC_MAX_MACHINES];
    int machine_count;
    bool browse_active;
};

// The link of the connection plugged into the network.  While none is, this
// points at an empty link that no session or browse can start on.
static ppc_link_t g_no_link;
static ppc_link_t *g_ppc = &g_no_link;

// ============================================================================
// Forward declarations
// ============================================================================

static void ppc_session_release(ppc_session_t *s, const char *reason, bool notify);
static void ppc_handle_message(ppc_session_t *s, const uint8_t *msg, int len);
static void ppc_start_browse_session(const ppc_machine_t *m);

// ============================================================================
// Operations — field helpers
// ============================================================================

// Pascal strings: one length byte then the characters, the whole field
// transmitted whether used or not (§1).
static void put_pstring(uint8_t *dst, size_t field_size, const char *s) {
    memset(dst, 0, field_size);
    size_t n = s ? strlen(s) : 0;
    size_t max = field_size - 1;
    if (n > max)
        n = max;
    dst[0] = (uint8_t)n;
    if (n)
        memcpy(dst + 1, s, n);
}

static void get_pstring(const uint8_t *src, size_t field_size, char *out, size_t out_size) {
    size_t n = src[0];
    if (n > field_size - 1)
        n = field_size - 1;
    if (n > out_size - 1)
        n = out_size - 1;
    memcpy(out, src + 1, n);
    out[n] = '\0';
}

// A `PPCPortRec` (§2.1).  Ports we name are always the by-string kind.
static void put_port_rec(uint8_t *dst, const char *name, const char *type) {
    memset(dst, 0, PPC_PORT_REC_SIZE);
    WR_BE16(&dst[0], 0); // Roman script
    put_pstring(&dst[2], 33, name);
    WR_BE16(&dst[36], PPC_KIND_BY_STRING);
    put_pstring(&dst[38], 33, type);
}

static void get_port_rec(const uint8_t *src, char *name, size_t name_size, char *type, size_t type_size) {
    get_pstring(&src[2], 33, name, name_size);
    if (RD_BE16(&src[36]) == PPC_KIND_BY_STRING) {
        get_pstring(&src[38], 33, type, type_size);
    } else {
        // The creator-and-type variant overlays the same bytes (§2.1).
        snprintf(type, type_size, "%.4s/%.4s", (const char *)&src[38], (const char *)&src[42]);
    }
}

// A `LocationNameRec` naming this host as an NBP entity (§2.2).
static void put_location(uint8_t *dst, const char *object) {
    memset(dst, 0, PPC_LOCATION_SIZE);
    WR_BE16(&dst[0], PPC_LOC_NBP);
    put_pstring(&dst[2], 33, object);
    put_pstring(&dst[36], 33, PPC_NBP_TYPE);
    put_pstring(&dst[70], 33, "*");
}

// ============================================================================
// Operations — the session table
// ============================================================================

static ppc_session_t *ppc_alloc_session(void) {
    if (g_ppc == &g_no_link)
        return NULL; // no machine to hold a session with
    for (int i = 0; i < PPC_MAX_SESSIONS; i++) {
        ppc_session_t *s = &g_ppc->sessions[i];
        if (s->in_use)
            continue;
        memset(s, 0, sizeof(*s));
        s->in_use = true;
        s->slot = i;
        s->id = g_ppc->next_session_id++;
        s->rx = (uint8_t *)malloc(PPC_MAX_MESSAGE);
        if (!s->rx) {
            s->in_use = false;
            return NULL;
        }
        return s;
    }
    LOG(2, "PPC: session table full");
    return NULL;
}

// ADSP permits only one connection between a given pair of sockets (Inside
// AppleTalk 12-5), and a guest's end lingers briefly after we close ours, so
// every outgoing connection takes its own local socket — the same thing a
// real PPC Toolbox does by asking ADSP to assign one.
static uint8_t ppc_alloc_client_socket(void) {
    for (uint8_t sock = PPC_CLIENT_SOCKET; sock < PPC_CLIENT_SOCKET + PPC_MAX_SESSIONS * 2; sock++) {
        bool taken = false;
        for (int i = 0; i < PPC_MAX_SESSIONS; i++) {
            const ppc_session_t *s = &g_ppc->sessions[i];
            if (s->in_use && s->initiator && s->local_socket == sock) {
                taken = true;
                break;
            }
        }
        if (!taken)
            return sock;
    }
    return PPC_CLIENT_SOCKET;
}

static ppc_session_t *ppc_session_for_conn(adsp_conn_t *c) {
    for (int i = 0; i < PPC_MAX_SESSIONS; i++) {
        if (g_ppc->sessions[i].in_use && g_ppc->sessions[i].conn == c)
            return &g_ppc->sessions[i];
    }
    return NULL;
}

// End a session.  Closing the ADSP connection *is* the goodbye (§4.4), so the
// two always happen together; `ppc_adsp_close` clears `conn` first so a
// connection that died on its own is not closed twice.
static void ppc_session_release(ppc_session_t *s, const char *reason, bool notify) {
    if (!s || !s->in_use)
        return;
    const ppc_client_t *client = s->client;
    void *ctx = s->client_ctx;
    bool was_open = (s->state == PPC_SESSION_OPEN);
    adsp_conn_t *conn = s->conn;
    LOG(4, "PPC: session %u to '%s' ended — %s", (unsigned)s->id, s->port_name, reason ? reason : "closed");
    free(s->rx);
    s->rx = NULL;
    s->in_use = false;
    s->state = PPC_SESSION_FREE;
    s->conn = NULL;
    if (conn)
        adsp_close(atalk_adsp_stack(), conn, reason ? reason : "the PPC session ended");
    if (notify && client) {
        if (was_open && client->on_closed)
            client->on_closed(ctx, s);
        else if (!was_open && client->on_failed)
            client->on_failed(ctx, s, reason ? reason : "the session ended");
    }
}

// ============================================================================
// Operations — writing session messages
// ============================================================================

// Every session message is one ADSP client message, terminated by EOM (§4.1).
static int ppc_write_message(ppc_session_t *s, const uint8_t *msg, int len) {
    if (!s || !s->conn)
        return -1;
    if (adsp_write(atalk_adsp_stack(), s->conn, msg, len, true) < 0) {
        LOG(2, "PPC: session %u could not queue a %d-byte message", (unsigned)s->id, len);
        return -1;
    }
    s->bytes_out += (uint64_t)len;
    return 0;
}

// The session request of §4.3.  A zero-length user name asks for a guest
// session, which is the only kind we use.
static int ppc_write_session_request(ppc_session_t *s, const char *dest_port, const char *dest_type) {
    uint8_t blk[PPC_SESSION_REQUEST_SIZE];
    memset(blk, 0, sizeof(blk));
    WR_BE32(&blk[0], PPC_MSG_SREQ);
    WR_BE32(&blk[4], 0); // user data, handed to the far side's PPCInform client
    put_port_rec(&blk[8], g_host->port, PPC_NBP_TYPE);
    put_port_rec(&blk[80], dest_port, dest_type);
    put_location(&blk[152], g_host->port);
    put_pstring(&blk[256], 33, ""); // guest (§4.3)
    return ppc_write_message(s, blk, sizeof(blk));
}

static int ppc_write_accept(ppc_session_t *s) {
    uint8_t blk[PPC_ANSWER_SIZE];
    WR_BE32(&blk[0], PPC_MSG_SAPT);
    WR_BE32(&blk[4], 0);
    return ppc_write_message(s, blk, sizeof(blk));
}

static int ppc_write_reject(ppc_session_t *s, uint32_t reason) {
    uint8_t blk[PPC_ANSWER_SIZE];
    WR_BE32(&blk[0], PPC_MSG_SREJ);
    WR_BE32(&blk[4], reason);
    g_ppc->stats.sessions_refused++;
    LOG(3, "PPC: rejecting a session request, reason %u", (unsigned)reason);
    return ppc_write_message(s, blk, sizeof(blk));
}

// The list-ports request of §4.6, asking for everything from the start.  The
// one-character Pascal string "=" is the wildcard in both name and type.
static int ppc_write_list_request(ppc_session_t *s) {
    uint8_t blk[PPC_LIST_REQUEST_SIZE];
    memset(blk, 0, sizeof(blk));
    WR_BE32(&blk[0], PPC_MSG_LPRT);
    WR_BE16(&blk[4], 0); // start index: from the beginning
    WR_BE16(&blk[6], PPC_LIST_REQUEST_COUNT);
    put_port_rec(&blk[8], "=", "=");
    put_pstring(&blk[80], 33, "");
    return ppc_write_message(s, blk, sizeof(blk));
}

int atalk_ppc_send_block(ppc_session_t *s, uint32_t creator, uint32_t type, uint32_t user_data, const uint8_t *payload,
                         int len) {
    if (!s || !s->in_use || s->state != PPC_SESSION_OPEN)
        return -1;
    if (len < 0 || len > PPC_MAX_MESSAGE - PPC_BLOCK_HEADER_SIZE)
        return -1;
    uint8_t hdr[PPC_BLOCK_HEADER_SIZE];
    WR_BE32(&hdr[0], creator);
    WR_BE32(&hdr[4], type);
    WR_BE32(&hdr[8], user_data);

    // Header and payload are one client message, so they must not be split by
    // an EOM in between (§4.7).
    adsp_stack_t *stack = atalk_adsp_stack();
    if (adsp_write(stack, s->conn, hdr, PPC_BLOCK_HEADER_SIZE, false) < 0)
        return -1;
    if (adsp_write(stack, s->conn, payload, len, true) < 0)
        return -1;
    s->bytes_out += (uint64_t)(PPC_BLOCK_HEADER_SIZE + len);
    g_ppc->stats.blocks_out++;
    return 0;
}

// ============================================================================
// Operations — the browse (§3, §4.6)
// ============================================================================

static void ppc_ports_clear(void) {
    g_ppc->port_count = 0;
    memset(g_ppc->ports, 0, sizeof(g_ppc->ports));
}

static void ppc_port_add(const char *machine, uint8_t node, uint8_t socket, const char *name, const char *type,
                         bool auth_required) {
    // Ports are keyed by (machine, name): a re-browse refreshes in place
    // rather than growing the table.
    for (int i = 0; i < g_ppc->port_count; i++) {
        if (!strcmp(g_ppc->ports[i].machine, machine) && !strcmp(g_ppc->ports[i].name, name)) {
            g_ppc->ports[i].node = node;
            g_ppc->ports[i].socket = socket;
            g_ppc->ports[i].auth_required = auth_required;
            snprintf(g_ppc->ports[i].type, sizeof(g_ppc->ports[i].type), "%s", type);
            return;
        }
    }
    if (g_ppc->port_count >= PPC_MAX_PORTS) {
        LOG(2, "PPC: port table full, dropping '%s'", name);
        return;
    }
    ppc_port_info_t *p = &g_ppc->ports[g_ppc->port_count++];
    snprintf(p->machine, sizeof(p->machine), "%s", machine);
    snprintf(p->name, sizeof(p->name), "%s", name);
    snprintf(p->type, sizeof(p->type), "%s", type);
    p->node = node;
    p->socket = socket;
    p->auth_required = auth_required;
    LOG(3, "PPC: discovered port '%s' (%s) on '%s' at %u:%u", p->name, p->type, p->machine, (unsigned)node,
        (unsigned)socket);
}

// Keep the collection in a stable order regardless of arrival: scripts assert
// on indices.
static int ppc_port_cmp(const void *a, const void *b) {
    const ppc_port_info_t *x = (const ppc_port_info_t *)a;
    const ppc_port_info_t *y = (const ppc_port_info_t *)b;
    if (x->node != y->node)
        return (int)x->node - (int)y->node;
    if (x->socket != y->socket)
        return (int)x->socket - (int)y->socket;
    return strcmp(x->name, y->name);
}

static void ppc_ports_sort(void) {
    if (g_ppc->port_count > 1)
        qsort(g_ppc->ports, (size_t)g_ppc->port_count, sizeof(g_ppc->ports[0]), ppc_port_cmp);
}

// One NBP reply tuple: a machine that speaks program linking (§3).
static void ppc_on_nbp_reply(void *ctx, const atalk_nbp_info_t *info) {
    (void)ctx;
    if (!info || !g_ppc->browse_active)
        return;
    if (info->node == LLAP_HOST_NODE)
        return; // that is our own advertisement
    for (int i = 0; i < g_ppc->machine_count; i++) {
        if (g_ppc->machines[i].node == info->node && g_ppc->machines[i].socket == info->socket)
            return; // already queried this round
    }
    if (g_ppc->machine_count >= PPC_MAX_MACHINES)
        return;
    ppc_machine_t *m = &g_ppc->machines[g_ppc->machine_count++];
    snprintf(m->name, sizeof(m->name), "%s", info->object);
    m->node = info->node;
    m->socket = info->socket;
    LOG(3, "PPC: '%s' answers at %u:%u — asking for its ports", m->name, (unsigned)m->node, (unsigned)m->socket);
    ppc_start_browse_session(m);
}

int atalk_ppc_browse(char *err, size_t err_len) {
    if (!atalk_get_enabled()) {
        snprintf(err, err_len, "the AppleTalk stack is detached from the link");
        return -1;
    }
    g_ppc->browse_active = true;
    g_ppc->machine_count = 0;
    ppc_ports_clear();
    g_ppc->stats.browses++;
    // Every program-linking machine registers one entity of this type (§3).
    if (atalk_nbp_lookup("=", PPC_NBP_TYPE, "*", PPC_CLIENT_SOCKET, ppc_on_nbp_reply, NULL) != 0) {
        g_ppc->browse_active = false;
        snprintf(err, err_len, "the NBP lookup could not be sent");
        return -1;
    }
    return 0;
}

bool atalk_ppc_browse_in_flight(void) {
    if (!g_ppc->browse_active)
        return false;
    for (int i = 0; i < PPC_MAX_SESSIONS; i++)
        if (g_ppc->sessions[i].in_use && g_ppc->sessions[i].use == PPC_USE_BROWSE)
            return true;
    return false;
}

int atalk_ppc_port_count(void) {
    return g_ppc->port_count;
}

bool atalk_ppc_port_info(int index, ppc_port_info_t *out) {
    if (index < 0 || index >= g_ppc->port_count || !out)
        return false;
    *out = g_ppc->ports[index];
    return true;
}

int atalk_ppc_port_find(const char *name) {
    if (!name)
        return -1;
    for (int i = 0; i < g_ppc->port_count; i++)
        if (!strcmp(g_ppc->ports[i].name, name))
            return i;
    return -1;
}

// ============================================================================
// Operations — inbound message dispatch
// ============================================================================

// A list-ports reply batch: zero or more PortInfoRecs, or the 6-byte trailer
// that ends the enumeration (§4.6).
static void ppc_handle_browse_reply(ppc_session_t *s, const uint8_t *msg, int len) {
    if (len >= 4 && RD_BE32(msg) == PPC_MSG_LRSP) {
        int actual = (len >= 6) ? RD_BE16(&msg[4]) : s->ports_collected;
        LOG(3, "PPC: '%s' listed %d port(s)", s->machine, actual);
        ppc_ports_sort();
        ppc_session_release(s, "the port list is complete", false);
        return;
    }
    if (len % PPC_PORT_INFO_SIZE != 0) {
        ppc_session_release(s, "the port list reply was not a whole number of entries", false);
        return;
    }
    for (int off = 0; off + PPC_PORT_INFO_SIZE <= len; off += PPC_PORT_INFO_SIZE) {
        const uint8_t *e = msg + off;
        char name[33] = "", type[33] = "";
        get_port_rec(&e[2], name, sizeof(name), type, sizeof(type));
        if (!name[0])
            continue;
        ppc_port_add(s->machine, s->peer_node, s->peer_socket, name, type, e[1] != 0);
        s->ports_collected++;
    }
    ppc_ports_sort();
}

// The answer to our session request (§4.4).
static void ppc_handle_session_answer(ppc_session_t *s, const uint8_t *msg, int len) {
    if (len < 4) {
        ppc_session_release(s, "the far side answered with a runt block", true);
        return;
    }
    uint32_t kind = RD_BE32(msg);
    uint32_t detail = (len >= 8) ? RD_BE32(&msg[4]) : 0;

    switch (kind) {
    case PPC_MSG_SAPT:
        s->state = PPC_SESSION_OPEN;
        g_ppc->stats.sessions_opened++;
        LOG(3, "PPC: session %u to '%s' accepted", (unsigned)s->id, s->port_name);
        if (s->client && s->client->on_ready)
            s->client->on_ready(s->client_ctx, s);
        return;
    case PPC_MSG_SREJ: {
        // The reason codes of §4.2, as messages a script can act on.
        static const char *const REASONS[] = {
            "the far side rejected the session",
            "that program is not shared over the network",
            "there is no such program-linking port on that machine",
            "the guest user is not known to that machine",
            "authentication failed",
            "that machine has no program waiting for a link",
            "guest program linking is not enabled on that machine",
            "program linking is switched off on that machine",
        };
        const char *why = (detail < ARRAY_LEN(REASONS)) ? REASONS[detail] : "the far side rejected the session";
        g_ppc->stats.sessions_rejected++;
        ppc_session_release(s, why, true);
        return;
    }
    case PPC_MSG_UREJ: {
        char why[96];
        snprintf(why, sizeof(why), "the program refused the link (code %u)", (unsigned)detail);
        g_ppc->stats.sessions_rejected++;
        ppc_session_release(s, why, true);
        return;
    }
    case PPC_MSG_ACNT:
        // Authenticated linking; we only speak guest (§4.5).
        g_ppc->stats.sessions_rejected++;
        ppc_session_release(s, "that port requires an authenticated link, which is not implemented", true);
        return;
    default:
        ppc_session_release(s, "the far side is not speaking the PPC session protocol", true);
        return;
    }
}

// A session request arriving at our host port (§4.4, responder side).
static void ppc_handle_session_request(ppc_session_t *s, const uint8_t *msg, int len) {
    if (len < PPC_SESSION_REQUEST_SIZE) {
        ppc_write_reject(s, PPC_REJECT_UNKNOWN_PORT);
        ppc_session_release(s, "the session request was short", false);
        return;
    }
    char dest_name[33] = "", dest_type[33] = "";
    char src_name[33] = "";
    char unused_type[33] = "";
    get_port_rec(&msg[8], src_name, sizeof(src_name), unused_type, sizeof(unused_type));
    get_port_rec(&msg[80], dest_name, sizeof(dest_name), dest_type, sizeof(dest_type));
    char user[33] = "";
    get_pstring(&msg[256], 33, user, sizeof(user));

    if (!g_host->enabled) {
        ppc_write_reject(s, PPC_REJECT_LINKING_OFF);
        ppc_session_release(s, "program linking is switched off here", false);
        return;
    }
    if (strcmp(dest_name, g_host->port) != 0) {
        LOG(3, "PPC: session request for unknown port '%s' (we are '%s')", dest_name, g_host->port);
        ppc_write_reject(s, PPC_REJECT_UNKNOWN_PORT);
        ppc_session_release(s, "no such port here", false);
        return;
    }
    if (user[0]) {
        // Guest linking only (§4.5).
        ppc_write_reject(s, PPC_REJECT_UNKNOWN_USER);
        ppc_session_release(s, "only guest links are accepted here", false);
        return;
    }

    snprintf(s->port_name, sizeof(s->port_name), "%s", src_name[0] ? src_name : dest_name);
    s->state = PPC_SESSION_OPEN;
    g_ppc->stats.sessions_opened++;
    LOG(3, "PPC: accepted a guest session from '%s' on node %u", s->port_name, (unsigned)s->peer_node);
    if (ppc_write_accept(s) != 0) {
        ppc_session_release(s, "the acceptance could not be sent", true);
        return;
    }
    if (s->client && s->client->on_ready)
        s->client->on_ready(s->client_ctx, s);
}

// A list-ports request arriving at our host port: we publish exactly one port
// (§4.6).
static void ppc_handle_list_request(ppc_session_t *s) {
    uint8_t entry[PPC_PORT_INFO_SIZE];
    memset(entry, 0, sizeof(entry));
    entry[0] = 0;
    entry[1] = 0; // guest links are welcome, so no authentication is required
    put_port_rec(&entry[2], g_host->port, PPC_NBP_TYPE);
    if (g_host->enabled)
        ppc_write_message(s, entry, sizeof(entry));

    uint8_t trailer[PPC_LIST_TRAILER_SIZE];
    WR_BE32(&trailer[0], PPC_MSG_LRSP);
    WR_BE16(&trailer[4], g_host->enabled ? 1 : 0);
    ppc_write_message(s, trailer, sizeof(trailer));
}

// One fully reassembled message.
static void ppc_handle_message(ppc_session_t *s, const uint8_t *msg, int len) {
    if (s->use == PPC_USE_BROWSE) {
        ppc_handle_browse_reply(s, msg, len);
        return;
    }
    if (s->state == PPC_SESSION_REQUESTED) {
        ppc_handle_session_answer(s, msg, len);
        return;
    }
    if (s->state != PPC_SESSION_OPEN) {
        // The responder's first message decides what this session is.
        if (len >= 4 && RD_BE32(msg) == PPC_MSG_LPRT) {
            ppc_handle_list_request(s);
            return;
        }
        if (len >= 4 && RD_BE32(msg) == PPC_MSG_SREQ) {
            ppc_handle_session_request(s, msg, len);
            return;
        }
        ppc_session_release(s, "the first message was not a session or list request", false);
        return;
    }

    // An open session carries message blocks (§4.7).
    if (len < PPC_BLOCK_HEADER_SIZE) {
        g_ppc->stats.malformed++;
        LOG(3, "PPC: session %u sent a %d-byte block, shorter than its header", (unsigned)s->id, len);
        return;
    }
    uint32_t creator = RD_BE32(&msg[0]);
    uint32_t type = RD_BE32(&msg[4]);
    uint32_t user_data = RD_BE32(&msg[8]);
    g_ppc->stats.blocks_in++;
    if (s->client && s->client->on_block)
        s->client->on_block(s->client_ctx, s, creator, type, user_data, msg + PPC_BLOCK_HEADER_SIZE,
                            len - PPC_BLOCK_HEADER_SIZE);
}

// ============================================================================
// Operations — ADSP client callbacks
// ============================================================================

static void ppc_adsp_open(void *ctx, adsp_conn_t *c) {
    (void)ctx;
    ppc_session_t *s = ppc_session_for_conn(c);
    if (!s)
        return;
    if (!s->initiator)
        return; // the guest speaks first

    if (s->use == PPC_USE_BROWSE) {
        if (ppc_write_list_request(s) != 0)
            ppc_session_release(s, "the port list request could not be sent", false);
        return;
    }
    // Ask for the session; the answer arrives as the next message (§4.4).
    s->state = PPC_SESSION_REQUESTED;
    char type[33] = "";
    int idx = atalk_ppc_port_find(s->port_name);
    if (idx >= 0)
        snprintf(type, sizeof(type), "%s", g_ppc->ports[idx].type);
    if (ppc_write_session_request(s, s->port_name, type) != 0)
        ppc_session_release(s, "the session request could not be sent", true);
}

static void ppc_adsp_data(void *ctx, adsp_conn_t *c, const uint8_t *data, int len, bool eom) {
    (void)ctx;
    ppc_session_t *s = ppc_session_for_conn(c);
    if (!s || !s->rx)
        return;
    if (len > 0) {
        if (s->rx_len + len > PPC_MAX_MESSAGE) {
            ppc_session_release(s, "the far side sent a message larger than we will reassemble", true);
            return;
        }
        memcpy(s->rx + s->rx_len, data, (size_t)len);
        s->rx_len += len;
        s->bytes_in += (uint64_t)len;
    }
    if (!eom)
        return; // a message is complete only at the marker (§4.1)

    // Hand the buffer off before dispatching: a client may end the session
    // from inside its callback, and the payload it is reading must outlive
    // that.  The session gets a fresh buffer for the next message.
    uint8_t *msg = s->rx;
    int msg_len = s->rx_len;
    uint8_t *fresh = (uint8_t *)malloc(PPC_MAX_MESSAGE);
    if (!fresh) {
        ppc_session_release(s, "out of memory reassembling a message", true);
        return;
    }
    s->rx = fresh;
    s->rx_len = 0;
    ppc_handle_message(s, msg, msg_len);
    free(msg);
}

static void ppc_adsp_close(void *ctx, adsp_conn_t *c, const char *reason) {
    (void)ctx;
    ppc_session_t *s = ppc_session_for_conn(c);
    if (!s)
        return;
    s->conn = NULL; // the engine has already freed it
    ppc_session_release(s, reason ? reason : "the connection closed", true);
}

// A guest opening a connection to our listening socket: give it a session
// slot and wait for its first message.
static bool ppc_adsp_accept(void *ctx, const atalk_socket_addr_t *from) {
    (void)ctx;
    if (!g_host->enabled) {
        LOG(3, "PPC: refusing a connection from node %u — the host port is off", (unsigned)from->node);
        return false;
    }
    return true;
}

static void ppc_adsp_open_inbound(void *ctx, adsp_conn_t *c) {
    (void)ctx;
    if (ppc_session_for_conn(c))
        return;
    ppc_session_t *s = ppc_alloc_session();
    if (!s) {
        adsp_close(atalk_adsp_stack(), c, "no PPC session slot is free");
        return;
    }
    const atalk_socket_addr_t *peer = adsp_conn_remote(c);
    s->conn = c;
    s->initiator = false;
    s->use = PPC_USE_SESSION;
    s->state = PPC_SESSION_CONNECTING;
    s->peer_node = peer ? peer->node : 0;
    s->peer_socket = peer ? peer->socket : 0;
    s->client = g_host->inbound_client;
    s->client_ctx = g_host->inbound_ctx;
    LOG(4, "PPC: connection from node %u accepted as session %u", (unsigned)s->peer_node, (unsigned)s->id);
}

// The listener's client: inbound connections are adopted here.
static const adsp_client_t g_listen_client = {
    .on_accept = ppc_adsp_accept,
    .on_open = ppc_adsp_open_inbound,
    .on_data = ppc_adsp_data,
    .on_close = ppc_adsp_close,
};

// Connections we open ourselves.
static const adsp_client_t g_dial_client = {
    .on_open = ppc_adsp_open,
    .on_data = ppc_adsp_data,
    .on_close = ppc_adsp_close,
};

// ============================================================================
// Operations — opening sessions
// ============================================================================

static void ppc_start_browse_session(const ppc_machine_t *m) {
    ppc_session_t *s = ppc_alloc_session();
    if (!s)
        return;
    s->initiator = true;
    s->use = PPC_USE_BROWSE;
    s->state = PPC_SESSION_CONNECTING;
    s->peer_node = m->node;
    s->peer_socket = m->socket;
    snprintf(s->machine, sizeof(s->machine), "%s", m->name);
    snprintf(s->port_name, sizeof(s->port_name), "(browse)");

    atalk_socket_addr_t dest = {.net = 0, .node = m->node, .socket = m->socket};
    s->local_socket = ppc_alloc_client_socket();
    s->conn = adsp_open(atalk_adsp_stack(), &dest, s->local_socket, &g_dial_client, NULL);
    if (!s->conn)
        ppc_session_release(s, "no ADSP connection was available", false);
}

ppc_session_t *atalk_ppc_open(const char *port_name, const ppc_client_t *client, void *ctx, char *err, size_t err_len) {
    if (!port_name || !*port_name) {
        snprintf(err, err_len, "no target port was named");
        return NULL;
    }
    int idx = atalk_ppc_port_find(port_name);
    if (idx < 0) {
        snprintf(err, err_len, "no program-linking port named '%s' has been discovered", port_name);
        return NULL;
    }
    if (g_ppc->ports[idx].auth_required) {
        snprintf(err, err_len, "'%s' requires an authenticated link, which is not implemented", port_name);
        return NULL;
    }
    ppc_session_t *s = ppc_alloc_session();
    if (!s) {
        snprintf(err, err_len, "no PPC session slot is free");
        return NULL;
    }
    s->initiator = true;
    s->use = PPC_USE_SESSION;
    s->state = PPC_SESSION_CONNECTING;
    s->peer_node = g_ppc->ports[idx].node;
    s->peer_socket = g_ppc->ports[idx].socket;
    s->client = client;
    s->client_ctx = ctx;
    snprintf(s->port_name, sizeof(s->port_name), "%s", port_name);
    snprintf(s->machine, sizeof(s->machine), "%s", g_ppc->ports[idx].machine);

    atalk_socket_addr_t dest = {.net = 0, .node = s->peer_node, .socket = s->peer_socket};
    s->local_socket = ppc_alloc_client_socket();
    s->conn = adsp_open(atalk_adsp_stack(), &dest, s->local_socket, &g_dial_client, NULL);
    if (!s->conn) {
        ppc_session_release(s, "no ADSP connection was available", false);
        snprintf(err, err_len, "no ADSP connection was available");
        return NULL;
    }
    LOG(4, "PPC: session %u opening to '%s' on node %u", (unsigned)s->id, port_name, (unsigned)s->peer_node);
    return s;
}

ppc_session_t *atalk_ppc_find_open(const char *port_name) {
    if (!port_name)
        return NULL;
    for (int i = 0; i < PPC_MAX_SESSIONS; i++) {
        ppc_session_t *s = &g_ppc->sessions[i];
        if (s->in_use && s->use == PPC_USE_SESSION && s->state == PPC_SESSION_OPEN && !strcmp(s->port_name, port_name))
            return s;
    }
    return NULL;
}

void atalk_ppc_close(ppc_session_t *s, const char *reason) {
    ppc_session_release(s, reason ? reason : "closed locally", false);
}

void atalk_ppc_close_all(const char *reason) {
    for (int i = 0; i < PPC_MAX_SESSIONS; i++)
        if (g_ppc->sessions[i].in_use)
            atalk_ppc_close(&g_ppc->sessions[i], reason);
}

// ============================================================================
// Operations — the host port
// ============================================================================

// Accept guest sessions on the plugged-in connection's ADSP stack.
static int ppc_listen(void) {
    adsp_unlisten(atalk_adsp_stack(), PPC_HOST_SOCKET);
    return adsp_listen(atalk_adsp_stack(), PPC_HOST_SOCKET, &g_listen_client, NULL);
}

ppc_host_t *atalk_ppc_init(void) {
    ppc_host_t *host = calloc(1, sizeof(*host));
    if (!host)
        return NULL;
    snprintf(host->port, sizeof(host->port), "%s", PPC_HOST_PORT_DEFAULT);
    g_host = host;
    return host;
}

const char *atalk_ppc_host_port_name(void) {
    return g_host->port;
}

bool atalk_ppc_host_port_enabled(void) {
    return g_host->enabled;
}

int atalk_ppc_set_host_port(const char *name, bool enabled, char *err, size_t err_len) {
    const char *port = (name && *name) ? name : g_host->port;
    if (strlen(port) > 32) {
        snprintf(err, err_len, "a port name may be at most 32 characters");
        return -1;
    }
    if (!enabled) {
        atalk_nbp_withdraw(&g_host->nbp);
        if (port != g_host->port)
            snprintf(g_host->port, sizeof(g_host->port), "%s", port);
        g_host->enabled = false;
        // Sessions belong to the port; withdrawing it strands them.
        atalk_ppc_close_all("the host program-linking port was withdrawn");
        if (atalk_adsp_stack())
            adsp_unlisten(atalk_adsp_stack(), PPC_HOST_SOCKET);
        return 0;
    }

    // One NBP entity per machine, on the connection-listening socket (§3).
    // Published -- or renamed in place -- before the name is stored: a name
    // another machine holds leaves the port advertised as it was.  The old
    // advertisement was withdrawn first, so the port vanished.
    atalk_nbp_service_desc_t desc = {
        .object = port,
        .type = PPC_NBP_TYPE,
        .zone = "*",
        .socket = PPC_HOST_SOCKET,
        .node = LLAP_HOST_NODE,
        .net = 0,
    };
    if (atalk_nbp_publish(&g_host->nbp, &desc) != 0) {
        snprintf(err, err_len, "the name '%s' is already taken on the network", port);
        return -1;
    }
    if (port != g_host->port)
        snprintf(g_host->port, sizeof(g_host->port), "%s", port);
    if (!g_host->enabled) {
        // Accepting sessions needs a machine on the cable; one plugged in
        // later listens when it is (atalk_ppc_plug).
        if (atalk_adsp_stack() && ppc_listen() != 0) {
            atalk_nbp_withdraw(&g_host->nbp);
            snprintf(err, err_len, "the PPC listening socket could not be opened");
            return -1;
        }
        g_host->enabled = true;
    }
    LOG(3, "PPC: host port '%s' advertised as %s on socket %d", g_host->port, PPC_NBP_TYPE, PPC_HOST_SOCKET);
    return 0;
}

void atalk_ppc_set_inbound_client(const ppc_client_t *client, void *ctx) {
    g_host->inbound_client = client;
    g_host->inbound_ctx = ctx;
}

// ============================================================================
// Operations — accessors
// ============================================================================

int atalk_ppc_session_slot_max(void) {
    return PPC_MAX_SESSIONS;
}
ppc_session_t *atalk_ppc_session_at(int slot) {
    if (slot < 0 || slot >= PPC_MAX_SESSIONS || !g_ppc->sessions[slot].in_use)
        return NULL;
    return &g_ppc->sessions[slot];
}
ppc_session_state_t atalk_ppc_session_state(const ppc_session_t *s) {
    return s ? s->state : PPC_SESSION_FREE;
}
bool atalk_ppc_session_initiator(const ppc_session_t *s) {
    return s && s->initiator;
}
const char *atalk_ppc_session_port(const ppc_session_t *s) {
    return s ? s->port_name : "";
}
uint8_t atalk_ppc_session_peer_node(const ppc_session_t *s) {
    return s ? s->peer_node : 0;
}
uint64_t atalk_ppc_session_bytes_in(const ppc_session_t *s) {
    return s ? s->bytes_in : 0;
}
uint64_t atalk_ppc_session_bytes_out(const ppc_session_t *s) {
    return s ? s->bytes_out : 0;
}
uint32_t atalk_ppc_session_id(const ppc_session_t *s) {
    return s ? s->id : 0;
}

// ============================================================================
// Lifecycle
// ============================================================================

ppc_link_t *atalk_ppc_link_new(void) {
    ppc_link_t *link = calloc(1, sizeof(*link));
    if (link)
        link->next_session_id = 1;
    return link;
}

void atalk_ppc_link_free(ppc_link_t *link) {
    if (!link)
        return;
    GS_ASSERT(link != g_ppc);
    free(link);
}

void atalk_ppc_plug(ppc_link_t *link) {
    if (!link && g_ppc != &g_no_link) {
        // The Mac is gone: its sessions close (their ADSP connections are
        // still up and say goodbye), and a browse of it ends.
        atalk_ppc_close_all("the emulated machine is going away");
        g_ppc->browse_active = false;
        g_ppc->machine_count = 0;
        ppc_ports_clear();
    }
    g_ppc = link ? link : &g_no_link;
    // The published host port takes sessions on the new machine's stack.
    if (link && g_host->enabled && atalk_adsp_stack() && ppc_listen() != 0)
        LOG(1, "PPC: the host port cannot accept sessions on this machine");
}

// ============================================================================
// Object model — `appletalk.ppc`
// ============================================================================

static const class_desc_t ppc_class;
static const class_desc_t ppc_ports_class;
static const class_desc_t ppc_port_class;
static const class_desc_t ppc_sessions_class;
static const class_desc_t ppc_session_class;
static const class_desc_t ppc_stats_class;

static int ppc_obj_slot(struct object *self) {
    return object_entry_index(self);
}

// --- appletalk.ppc.ports[i] --------------------------------------------------

static DEF_GETTER(ppc_port_attr_name) {
    ppc_port_info_t info;
    return val_str(atalk_ppc_port_info(ppc_obj_slot(self), &info) ? info.name : "");
}
static DEF_GETTER(ppc_port_attr_type) {
    ppc_port_info_t info;
    return val_str(atalk_ppc_port_info(ppc_obj_slot(self), &info) ? info.type : "");
}
static DEF_GETTER(ppc_port_attr_machine) {
    ppc_port_info_t info;
    return val_str(atalk_ppc_port_info(ppc_obj_slot(self), &info) ? info.machine : "");
}
static DEF_GETTER(ppc_port_attr_node) {
    ppc_port_info_t info;
    return val_uint(1, atalk_ppc_port_info(ppc_obj_slot(self), &info) ? info.node : 0);
}
static DEF_GETTER(ppc_port_attr_socket) {
    ppc_port_info_t info;
    return val_uint(1, atalk_ppc_port_info(ppc_obj_slot(self), &info) ? info.socket : 0);
}
static DEF_GETTER(ppc_port_attr_auth) {
    ppc_port_info_t info;
    return val_bool(atalk_ppc_port_info(ppc_obj_slot(self), &info) ? info.auth_required : false);
}

static const member_t ppc_port_members[] = {
    {.kind = M_ATTR,
     .name = "name",
     .doc = "Port name as the guest's PPC browser shows it",
     .attr = {.type = V_STRING, .get = ppc_port_attr_name}            },
    {.kind = M_ATTR,
     .name = "type",
     .doc = "Port type string; applications use <signature>ep01",
     .attr = {.type = V_STRING, .get = ppc_port_attr_type}            },
    {.kind = M_ATTR,
     .name = "machine",
     .doc = "NBP name of the machine holding the port",
     .attr = {.type = V_STRING, .get = ppc_port_attr_machine}         },
    {.kind = M_ATTR,
     .name = "node",
     .doc = "LLAP node of that machine",
     .attr = {.type = V_UINT, .width = 1, .get = ppc_port_attr_node}  },
    {.kind = M_ATTR,
     .name = "socket",
     .doc = "Its PPC connection-listening socket",
     .attr = {.type = V_UINT, .width = 1, .get = ppc_port_attr_socket}},
    {.kind = M_ATTR,
     .name = "auth_required",
     .doc = "True if the port refuses guest links",
     .attr = {.type = V_BOOL, .get = ppc_port_attr_auth}              },
};

static const class_desc_t ppc_port_class = {
    .name = "ppc_port",
    .members = ppc_port_members,
    .n_members = ARRAY_LEN(ppc_port_members),
};

// --- appletalk.ppc.ports -----------------------------------------------------

static struct object *ppc_ports_get(struct object *self, int index) {
    (void)self;
    if (index < 0 || index >= atalk_ppc_port_count())
        return NULL;
    return object_cache_at(&g_host->port_entries, index, NULL);
}
static struct object *ppc_ports_lookup(struct object *self, const char *name) {
    (void)self;
    int idx = atalk_ppc_port_find(name);
    return (idx >= 0) ? object_cache_at(&g_host->port_entries, idx, NULL) : NULL;
}

static const collection_desc_t ppc_ports_entries = {
    .entry = &ppc_port_class,
    .by_index = {.get = ppc_ports_get, .slots = PPC_MAX_PORTS},
    .by_key = {.lookup = ppc_ports_lookup}
};

static const member_t ppc_ports_members[] = {
    OBJ_ENTRIES(&ppc_ports_entries, NULL),
};

static const class_desc_t ppc_ports_class = {
    .name = "ppc_ports",
    .doc = "Program-linking ports found on the network by browse",
    .members = ppc_ports_members,
    .n_members = ARRAY_LEN(ppc_ports_members),
};

// --- appletalk.ppc.sessions[i] -----------------------------------------------

static ppc_session_t *ppc_obj_session(struct object *self) {
    return atalk_ppc_session_at(ppc_obj_slot(self));
}

static DEF_GETTER(ppc_session_attr_id) {
    return val_uint(4, atalk_ppc_session_id(ppc_obj_session(self)));
}
static DEF_GETTER(ppc_session_attr_state) {
    int st = (int)atalk_ppc_session_state(ppc_obj_session(self));
    if (st < 0 || st >= PPC_SESSION_STATE_COUNT)
        st = 0;
    return val_enum(st, PPC_SESSION_STATE_NAMES, PPC_SESSION_STATE_COUNT);
}
static DEF_GETTER(ppc_session_attr_role) {
    return val_str(atalk_ppc_session_initiator(ppc_obj_session(self)) ? "initiator" : "responder");
}
static DEF_GETTER(ppc_session_attr_port) {
    return val_str(atalk_ppc_session_port(ppc_obj_session(self)));
}
static DEF_GETTER(ppc_session_attr_peer_node) {
    return val_uint(1, atalk_ppc_session_peer_node(ppc_obj_session(self)));
}
static DEF_GETTER(ppc_session_attr_bytes_in) {
    return val_uint(8, atalk_ppc_session_bytes_in(ppc_obj_session(self)));
}
static DEF_GETTER(ppc_session_attr_bytes_out) {
    return val_uint(8, atalk_ppc_session_bytes_out(ppc_obj_session(self)));
}

static const member_t ppc_session_members[] = {
    {.kind = M_ATTR,
     .name = "id",
     .doc = "Stable identity of this session",
     .attr = {.type = V_UINT, .width = 4, .get = ppc_session_attr_id}                               },
    {.kind = M_ATTR,
     .name = "state",
     .doc = "Session state",
     .attr = {.type = V_ENUM, .enum_values = PPC_SESSION_STATE_NAMES, .get = ppc_session_attr_state}},
    {.kind = M_ATTR,
     .name = "role",
     .doc = "initiator if we asked for the session, else responder",
     .attr = {.type = V_STRING, .get = ppc_session_attr_role}                                       },
    {.kind = M_ATTR,
     .name = "port",
     .doc = "The port at the far end",
     .attr = {.type = V_STRING, .get = ppc_session_attr_port}                                       },
    {.kind = M_ATTR,
     .name = "peer_node",
     .doc = "LLAP node of the far end",
     .attr = {.type = V_UINT, .width = 1, .get = ppc_session_attr_peer_node}                        },
    {.kind = M_ATTR,
     .name = "bytes_in",
     .doc = "Session bytes received",
     .attr = {.type = V_UINT, .width = 8, .get = ppc_session_attr_bytes_in}                         },
    {.kind = M_ATTR,
     .name = "bytes_out",
     .doc = "Session bytes sent",
     .attr = {.type = V_UINT, .width = 8, .get = ppc_session_attr_bytes_out}                        },
};

static const class_desc_t ppc_session_class = {
    .name = "ppc_session",
    .members = ppc_session_members,
    .n_members = ARRAY_LEN(ppc_session_members),
};

static struct object *ppc_sessions_get(struct object *self, int index) {
    (void)self;
    if (!atalk_ppc_session_at(index))
        return NULL;
    return object_cache_at(&g_host->session_entries, index, NULL);
}
// Name lookup by the port at the far end, so `sessions["Finder"].state` reads
// naturally in a script.
static struct object *ppc_sessions_lookup(struct object *self, const char *name) {
    (void)self;
    for (int i = 0; i < PPC_MAX_SESSIONS; i++) {
        const ppc_session_t *s = atalk_ppc_session_at(i);
        if (s && !strcmp(s->port_name, name))
            return object_cache_at(&g_host->session_entries, i, NULL);
    }
    return NULL;
}

static const collection_desc_t ppc_sessions_entries = {
    .entry = &ppc_session_class,
    .by_index = {.get = ppc_sessions_get, .slots = PPC_MAX_SESSIONS},
    .by_key = {.lookup = ppc_sessions_lookup}
};

static const member_t ppc_sessions_members[] = {
    OBJ_ENTRIES(&ppc_sessions_entries, NULL),
};

static const class_desc_t ppc_sessions_class = {
    .name = "ppc_sessions",
    .members = ppc_sessions_members,
    .n_members = ARRAY_LEN(ppc_sessions_members),
};

// --- appletalk.ppc.stats -----------------------------------------------------

// The plugged-in connection's counters: zero while none is.
static DEF_GETTER(ppc_stats_get) {
    return obj_u64_at(&g_ppc->stats, m);
}

static const member_t ppc_stats_members[] = {
    OBJ_U64_FIELD_WITH(ppc_stats_t, sessions_opened, "Sessions that reached the open state", ppc_stats_get),
    OBJ_U64_FIELD_WITH(ppc_stats_t, sessions_rejected, "Session requests the far side turned down", ppc_stats_get),
    OBJ_U64_FIELD_WITH(ppc_stats_t, sessions_refused, "Session requests we turned down", ppc_stats_get),
    OBJ_U64_FIELD_WITH(ppc_stats_t, blocks_in, "Message blocks received", ppc_stats_get),
    OBJ_U64_FIELD_WITH(ppc_stats_t, blocks_out, "Message blocks sent", ppc_stats_get),
    OBJ_U64_FIELD_WITH(ppc_stats_t, browses, "Port browses started", ppc_stats_get),
    OBJ_U64_FIELD_WITH(ppc_stats_t, malformed, "Message blocks discarded as malformed", ppc_stats_get),
};

static const class_desc_t ppc_stats_class = {
    .name = "ppc_stats",
    .members = ppc_stats_members,
    .n_members = ARRAY_LEN(ppc_stats_members),
};

// --- appletalk.ppc -----------------------------------------------------------

static DEF_METHOD(ppc_method_browse) {
    char err[192] = "";
    if (atalk_ppc_browse(err, sizeof(err)) != 0)
        return val_err("cannot browse for program-linking ports: %s", err);
    return val_none();
}

static DEF_GETTER(ppc_attr_browsing) {
    return val_bool(atalk_ppc_browse_in_flight());
}

static const member_t ppc_members[] = {
    {.kind = M_ATTR,
     .name = "browsing",
     .doc = "True while a port browse is still waiting on the network",
     .attr = {.type = V_BOOL, .get = ppc_attr_browsing}                                                    },
    {.kind = M_METHOD,
     .name = "browse",
     .doc = "Look for program-linking ports on the network; run the scheduler, then read `ports`",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = ppc_method_browse, .ui_flags = MM_MUTATE}},
};

static const class_desc_t ppc_class = {
    .name = "ppc",
    .doc = "Program-to-program communication: network ports, sessions, statistics",
    .members = ppc_members,
    .n_members = ARRAY_LEN(ppc_members),
};

void atalk_ppc_install_objects(struct object *parent) {
    struct object *ppc = object_new(&ppc_class, NULL, "ppc");
    if (!ppc)
        return;
    object_attach(parent, ppc);

    struct object *ports = object_new(&ppc_ports_class, NULL, "ports");
    if (ports)
        object_attach(ppc, ports);
    struct object *sessions = object_new(&ppc_sessions_class, NULL, "sessions");
    if (sessions) {
        object_set_category(sessions, M_CAT_ADVANCED);
        object_attach(ppc, sessions);
    }
    struct object *stats = object_new(&ppc_stats_class, NULL, "stats");
    if (stats) {
        object_set_category(stats, M_CAT_ADVANCED);
        object_attach(ppc, stats);
    }

    // The collection entry objects, made on first use.
    g_host->port_entries = (object_cache_t)OBJECT_CACHE(&ppc_port_class, NULL);
    g_host->session_entries = (object_cache_t)OBJECT_CACHE(&ppc_session_class, NULL);
    object_cache_set_parent(&g_host->port_entries, ports);
    object_cache_set_parent(&g_host->session_entries, sessions);
}
