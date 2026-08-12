// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_ppc.c
// PPC Toolbox session layer over ADSP.
//
// Coding reference: docs/core/network/ppc_appleevents.md — §2 for the record
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

LOG_USE_CATEGORY_NAME("ppc");

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

const char *const PPC_SESSION_STATE_NAMES[] = {"free", "connecting", "requested", "open", "failed"};

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

static ppc_session_t g_sessions[PPC_MAX_SESSIONS];
static ppc_port_info_t g_ports[PPC_MAX_PORTS];
static int g_port_count;
static uint32_t g_next_session_id = 1;

static char g_host_port[33] = "gs-host";
static bool g_host_enabled;
static atalk_nbp_entry_t *g_host_nbp;

static const ppc_client_t *g_inbound_client;
static void *g_inbound_ctx;

static ppc_stats_t g_stats;

// Machines the current browse has found but not yet queried.
typedef struct {
    char name[33];
    uint8_t node;
    uint8_t socket;
} ppc_machine_t;

#define PPC_MAX_MACHINES 8
static ppc_machine_t g_machines[PPC_MAX_MACHINES];
static int g_machine_count;
static bool g_browse_active;

// ============================================================================
// Forward declarations
// ============================================================================

static void ppc_session_release(ppc_session_t *s, const char *reason, bool notify);
static void ppc_handle_message(ppc_session_t *s, const uint8_t *msg, int len);
static void ppc_start_browse_session(const ppc_machine_t *m);

// ============================================================================
// Operations — field helpers
// ============================================================================

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint16_t get16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

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
    put16(&dst[0], 0); // Roman script
    put_pstring(&dst[2], 33, name);
    put16(&dst[36], PPC_KIND_BY_STRING);
    put_pstring(&dst[38], 33, type);
}

static void get_port_rec(const uint8_t *src, char *name, size_t name_size, char *type, size_t type_size) {
    get_pstring(&src[2], 33, name, name_size);
    if (get16(&src[36]) == PPC_KIND_BY_STRING) {
        get_pstring(&src[38], 33, type, type_size);
    } else {
        // The creator-and-type variant overlays the same bytes (§2.1).
        snprintf(type, type_size, "%.4s/%.4s", (const char *)&src[38], (const char *)&src[42]);
    }
}

// A `LocationNameRec` naming this host as an NBP entity (§2.2).
static void put_location(uint8_t *dst, const char *object) {
    memset(dst, 0, PPC_LOCATION_SIZE);
    put16(&dst[0], PPC_LOC_NBP);
    put_pstring(&dst[2], 33, object);
    put_pstring(&dst[36], 33, PPC_NBP_TYPE);
    put_pstring(&dst[70], 33, "*");
}

// ============================================================================
// Operations — the session table
// ============================================================================

static ppc_session_t *ppc_alloc_session(void) {
    for (int i = 0; i < PPC_MAX_SESSIONS; i++) {
        ppc_session_t *s = &g_sessions[i];
        if (s->in_use)
            continue;
        memset(s, 0, sizeof(*s));
        s->in_use = true;
        s->slot = i;
        s->id = g_next_session_id++;
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

static ppc_session_t *ppc_session_for_conn(adsp_conn_t *c) {
    for (int i = 0; i < PPC_MAX_SESSIONS; i++) {
        if (g_sessions[i].in_use && g_sessions[i].conn == c)
            return &g_sessions[i];
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
    uint8_t blk[PPC_START_BLK_SIZE];
    memset(blk, 0, sizeof(blk));
    put32(&blk[0], PPC_MSG_SREQ);
    put32(&blk[4], 0); // user data, handed to the far side's PPCInform client
    put_port_rec(&blk[8], g_host_port, PPC_NBP_TYPE);
    put_port_rec(&blk[80], dest_port, dest_type);
    put_location(&blk[152], g_host_port);
    put_pstring(&blk[256], 33, ""); // guest (§4.3)
    return ppc_write_message(s, blk, sizeof(blk));
}

static int ppc_write_accept(ppc_session_t *s) {
    uint8_t blk[PPC_ACCEPT_SIZE];
    put32(&blk[0], PPC_MSG_SAPT);
    put32(&blk[4], 0);
    return ppc_write_message(s, blk, sizeof(blk));
}

static int ppc_write_reject(ppc_session_t *s, uint32_t reason) {
    uint8_t blk[PPC_ACCEPT_SIZE];
    put32(&blk[0], PPC_MSG_SREJ);
    put32(&blk[4], reason);
    g_stats.sessions_refused++;
    LOG(3, "PPC: rejecting a session request, reason %u", (unsigned)reason);
    return ppc_write_message(s, blk, sizeof(blk));
}

// The list-ports request of §4.6, asking for everything from the start.  The
// one-character Pascal string "=" is the wildcard in both name and type.
static int ppc_write_list_request(ppc_session_t *s) {
    uint8_t blk[PPC_LIST_REQ_SIZE];
    memset(blk, 0, sizeof(blk));
    put32(&blk[0], PPC_MSG_LPRT);
    put16(&blk[4], 0); // start index: from the beginning
    put16(&blk[6], PPC_LIST_REQUEST_COUNT);
    put_port_rec(&blk[8], "=", "=");
    put_pstring(&blk[80], 33, "");
    return ppc_write_message(s, blk, sizeof(blk));
}

int atalk_ppc_send_block(ppc_session_t *s, uint32_t creator, uint32_t type, uint32_t user_data, const uint8_t *payload,
                         int len) {
    if (!s || !s->in_use || s->state != PPC_SESSION_OPEN)
        return -1;
    if (len < 0 || len > PPC_MAX_MESSAGE - PPC_HDR_BLK_SIZE)
        return -1;
    uint8_t hdr[PPC_HDR_BLK_SIZE];
    put32(&hdr[0], creator);
    put32(&hdr[4], type);
    put32(&hdr[8], user_data);

    // Header and payload are one client message, so they must not be split by
    // an EOM in between (§4.7).
    adsp_stack_t *stack = atalk_adsp_stack();
    if (adsp_write(stack, s->conn, hdr, PPC_HDR_BLK_SIZE, false) < 0)
        return -1;
    if (adsp_write(stack, s->conn, payload, len, true) < 0)
        return -1;
    s->bytes_out += (uint64_t)(PPC_HDR_BLK_SIZE + len);
    g_stats.blocks_out++;
    return 0;
}

// ============================================================================
// Operations — the browse (§3, §4.6)
// ============================================================================

static void ppc_ports_clear(void) {
    g_port_count = 0;
    memset(g_ports, 0, sizeof(g_ports));
}

static void ppc_port_add(const char *machine, uint8_t node, uint8_t socket, const char *name, const char *type,
                         bool auth_required) {
    // Ports are keyed by (machine, name): a re-browse refreshes in place
    // rather than growing the table.
    for (int i = 0; i < g_port_count; i++) {
        if (!strcmp(g_ports[i].machine, machine) && !strcmp(g_ports[i].name, name)) {
            g_ports[i].node = node;
            g_ports[i].socket = socket;
            g_ports[i].auth_required = auth_required;
            snprintf(g_ports[i].type, sizeof(g_ports[i].type), "%s", type);
            return;
        }
    }
    if (g_port_count >= PPC_MAX_PORTS) {
        LOG(2, "PPC: port table full, dropping '%s'", name);
        return;
    }
    ppc_port_info_t *p = &g_ports[g_port_count++];
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
// on indices (proposal §9.8).
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
    if (g_port_count > 1)
        qsort(g_ports, (size_t)g_port_count, sizeof(g_ports[0]), ppc_port_cmp);
}

// One NBP reply tuple: a machine that speaks program linking (§3).
static void ppc_on_nbp_reply(void *ctx, const atalk_nbp_info_t *info) {
    (void)ctx;
    if (!info || !g_browse_active)
        return;
    if (info->node == LLAP_HOST_NODE)
        return; // that is our own advertisement
    for (int i = 0; i < g_machine_count; i++) {
        if (g_machines[i].node == info->node && g_machines[i].socket == info->socket)
            return; // already queried this round
    }
    if (g_machine_count >= PPC_MAX_MACHINES)
        return;
    ppc_machine_t *m = &g_machines[g_machine_count++];
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
    g_browse_active = true;
    g_machine_count = 0;
    ppc_ports_clear();
    g_stats.browses++;
    // Every program-linking machine registers one entity of this type (§3).
    if (atalk_nbp_lookup("=", PPC_NBP_TYPE, "*", PPC_CLIENT_SOCKET, ppc_on_nbp_reply, NULL) != 0) {
        g_browse_active = false;
        snprintf(err, err_len, "the NBP lookup could not be sent");
        return -1;
    }
    return 0;
}

bool atalk_ppc_browse_in_flight(void) {
    if (!g_browse_active)
        return false;
    for (int i = 0; i < PPC_MAX_SESSIONS; i++)
        if (g_sessions[i].in_use && g_sessions[i].use == PPC_USE_BROWSE)
            return true;
    return false;
}

int atalk_ppc_port_count(void) {
    return g_port_count;
}

bool atalk_ppc_port_info(int index, ppc_port_info_t *out) {
    if (index < 0 || index >= g_port_count || !out)
        return false;
    *out = g_ports[index];
    return true;
}

int atalk_ppc_port_find(const char *name) {
    if (!name)
        return -1;
    for (int i = 0; i < g_port_count; i++)
        if (!strcmp(g_ports[i].name, name))
            return i;
    return -1;
}

// ============================================================================
// Operations — inbound message dispatch
// ============================================================================

// A list-ports reply batch: zero or more PortInfoRecs, or the 6-byte trailer
// that ends the enumeration (§4.6).
static void ppc_handle_browse_reply(ppc_session_t *s, const uint8_t *msg, int len) {
    if (len >= 4 && get32(msg) == PPC_MSG_LRSP) {
        int actual = (len >= 6) ? get16(&msg[4]) : s->ports_collected;
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
    uint32_t kind = get32(msg);
    uint32_t detail = (len >= 8) ? get32(&msg[4]) : 0;

    switch (kind) {
    case PPC_MSG_SAPT:
        s->state = PPC_SESSION_OPEN;
        g_stats.sessions_opened++;
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
        g_stats.sessions_rejected++;
        ppc_session_release(s, why, true);
        return;
    }
    case PPC_MSG_UREJ: {
        char why[96];
        snprintf(why, sizeof(why), "the program refused the link (code %u)", (unsigned)detail);
        g_stats.sessions_rejected++;
        ppc_session_release(s, why, true);
        return;
    }
    case PPC_MSG_ACNT:
        // Authenticated linking; we only speak guest (§4.5).
        g_stats.sessions_rejected++;
        ppc_session_release(s, "that port requires an authenticated link, which is not implemented", true);
        return;
    default:
        ppc_session_release(s, "the far side is not speaking the PPC session protocol", true);
        return;
    }
}

// A session request arriving at our host port (§4.4, responder side).
static void ppc_handle_session_request(ppc_session_t *s, const uint8_t *msg, int len) {
    if (len < PPC_START_BLK_SIZE) {
        ppc_write_reject(s, PPC_REJECT_NO_PORT);
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

    if (!g_host_enabled) {
        ppc_write_reject(s, PPC_REJECT_IAC_DISABLED);
        ppc_session_release(s, "program linking is switched off here", false);
        return;
    }
    if (strcmp(dest_name, g_host_port) != 0) {
        LOG(3, "PPC: session request for unknown port '%s' (we are '%s')", dest_name, g_host_port);
        ppc_write_reject(s, PPC_REJECT_NO_PORT);
        ppc_session_release(s, "no such port here", false);
        return;
    }
    if (user[0]) {
        // Guest linking only (§4.5).
        ppc_write_reject(s, PPC_REJECT_NO_USER_REC);
        ppc_session_release(s, "only guest links are accepted here", false);
        return;
    }

    snprintf(s->port_name, sizeof(s->port_name), "%s", src_name[0] ? src_name : dest_name);
    s->state = PPC_SESSION_OPEN;
    g_stats.sessions_opened++;
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
    put_port_rec(&entry[2], g_host_port, PPC_NBP_TYPE);
    if (g_host_enabled)
        ppc_write_message(s, entry, sizeof(entry));

    uint8_t trailer[PPC_LIST_RESP_SIZE];
    put32(&trailer[0], PPC_MSG_LRSP);
    put16(&trailer[4], g_host_enabled ? 1 : 0);
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
        if (len >= 4 && get32(msg) == PPC_MSG_LPRT) {
            ppc_handle_list_request(s);
            return;
        }
        if (len >= 4 && get32(msg) == PPC_MSG_SREQ) {
            ppc_handle_session_request(s, msg, len);
            return;
        }
        ppc_session_release(s, "the first message was not a session or list request", false);
        return;
    }

    // An open session carries message blocks (§4.7).
    if (len < PPC_HDR_BLK_SIZE) {
        LOG(3, "PPC: session %u sent a %d-byte block, shorter than its header", (unsigned)s->id, len);
        return;
    }
    uint32_t creator = get32(&msg[0]);
    uint32_t type = get32(&msg[4]);
    uint32_t user_data = get32(&msg[8]);
    g_stats.blocks_in++;
    if (s->client && s->client->on_block)
        s->client->on_block(s->client_ctx, s, creator, type, user_data, msg + PPC_HDR_BLK_SIZE, len - PPC_HDR_BLK_SIZE);
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
        snprintf(type, sizeof(type), "%s", g_ports[idx].type);
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
    if (!g_host_enabled) {
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
    s->client = g_inbound_client;
    s->client_ctx = g_inbound_ctx;
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
    s->conn = adsp_open(atalk_adsp_stack(), &dest, PPC_CLIENT_SOCKET, &g_dial_client, NULL);
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
    if (g_ports[idx].auth_required) {
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
    s->peer_node = g_ports[idx].node;
    s->peer_socket = g_ports[idx].socket;
    s->client = client;
    s->client_ctx = ctx;
    snprintf(s->port_name, sizeof(s->port_name), "%s", port_name);
    snprintf(s->machine, sizeof(s->machine), "%s", g_ports[idx].machine);

    atalk_socket_addr_t dest = {.net = 0, .node = s->peer_node, .socket = s->peer_socket};
    s->conn = adsp_open(atalk_adsp_stack(), &dest, PPC_CLIENT_SOCKET, &g_dial_client, NULL);
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
        ppc_session_t *s = &g_sessions[i];
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
        if (g_sessions[i].in_use)
            atalk_ppc_close(&g_sessions[i], reason);
}

// ============================================================================
// Operations — the host port
// ============================================================================

const char *atalk_ppc_host_port_name(void) {
    return g_host_port;
}

int atalk_ppc_set_host_port(const char *name, bool enabled, char *err, size_t err_len) {
    if (name && *name) {
        if (strlen(name) > 32) {
            snprintf(err, err_len, "a port name may be at most 32 characters");
            return -1;
        }
        snprintf(g_host_port, sizeof(g_host_port), "%s", name);
    }

    if (g_host_nbp) {
        atalk_nbp_unregister(g_host_nbp);
        g_host_nbp = NULL;
    }
    g_host_enabled = enabled;
    if (!enabled) {
        // Sessions belong to the port; withdrawing it strands them.
        atalk_ppc_close_all("the host program-linking port was withdrawn");
        adsp_unlisten(atalk_adsp_stack(), PPC_HOST_SOCKET);
        return 0;
    }

    // One NBP entity per machine, on the connection-listening socket (§3).
    atalk_nbp_service_desc_t desc = {
        .object = g_host_port,
        .type = PPC_NBP_TYPE,
        .zone = "*",
        .socket = PPC_HOST_SOCKET,
        .node = LLAP_HOST_NODE,
        .net = 0,
    };
    if (atalk_nbp_register(&desc, &g_host_nbp) != 0) {
        g_host_enabled = false;
        snprintf(err, err_len, "the name '%s' is already taken on the network", g_host_port);
        return -1;
    }
    adsp_unlisten(atalk_adsp_stack(), PPC_HOST_SOCKET);
    if (adsp_listen(atalk_adsp_stack(), PPC_HOST_SOCKET, &g_listen_client, NULL) != 0) {
        atalk_nbp_unregister(g_host_nbp);
        g_host_nbp = NULL;
        g_host_enabled = false;
        snprintf(err, err_len, "the PPC listening socket could not be opened");
        return -1;
    }
    LOG(3, "PPC: host port '%s' advertised as %s on socket %d", g_host_port, PPC_NBP_TYPE, PPC_HOST_SOCKET);
    return 0;
}

void atalk_ppc_set_inbound_client(const ppc_client_t *client, void *ctx) {
    g_inbound_client = client;
    g_inbound_ctx = ctx;
}

// ============================================================================
// Operations — accessors
// ============================================================================

int atalk_ppc_session_slot_max(void) {
    return PPC_MAX_SESSIONS;
}
ppc_session_t *atalk_ppc_session_at(int slot) {
    if (slot < 0 || slot >= PPC_MAX_SESSIONS || !g_sessions[slot].in_use)
        return NULL;
    return &g_sessions[slot];
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
const ppc_stats_t *atalk_ppc_get_stats(void) {
    return &g_stats;
}

// ============================================================================
// Lifecycle
// ============================================================================

void atalk_ppc_init(void) {
    atalk_ppc_shutdown();
    memset(&g_stats, 0, sizeof(g_stats));
    g_next_session_id = 1;
    snprintf(g_host_port, sizeof(g_host_port), "gs-host");
}

void atalk_ppc_shutdown(void) {
    for (int i = 0; i < PPC_MAX_SESSIONS; i++) {
        if (g_sessions[i].in_use) {
            free(g_sessions[i].rx);
            memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
        }
    }
    if (g_host_nbp) {
        atalk_nbp_unregister(g_host_nbp);
        g_host_nbp = NULL;
    }
    g_host_enabled = false;
    g_browse_active = false;
    g_machine_count = 0;
    ppc_ports_clear();
    atalk_nbp_lookup_cancel();
}

// ============================================================================
// Object model — `appletalk.ppc`
// ============================================================================

static struct object *g_ppc_object;
static struct object *g_ppc_ports_object;
static struct object *g_ppc_sessions_object;
static struct object *g_ppc_stats_object;

typedef struct {
    int slot;
} ppc_slot_data_t;

static ppc_slot_data_t g_ppc_port_data[PPC_MAX_PORTS];
static struct object *g_ppc_port_objs[PPC_MAX_PORTS];
static ppc_slot_data_t g_ppc_session_data[PPC_MAX_SESSIONS];
static struct object *g_ppc_session_objs[PPC_MAX_SESSIONS];

extern const class_desc_t ppc_class;
extern const class_desc_t ppc_ports_class;
extern const class_desc_t ppc_port_class;
extern const class_desc_t ppc_sessions_class;
extern const class_desc_t ppc_session_class;
extern const class_desc_t ppc_stats_class;

static int ppc_obj_slot(struct object *self) {
    const ppc_slot_data_t *d = (const ppc_slot_data_t *)object_data(self);
    return d ? d->slot : -1;
}

// --- appletalk.ppc.ports[i] --------------------------------------------------

static value_t ppc_port_attr_name(struct object *self, const member_t *m) {
    (void)m;
    ppc_port_info_t info;
    return val_str(atalk_ppc_port_info(ppc_obj_slot(self), &info) ? info.name : "");
}
static value_t ppc_port_attr_type(struct object *self, const member_t *m) {
    (void)m;
    ppc_port_info_t info;
    return val_str(atalk_ppc_port_info(ppc_obj_slot(self), &info) ? info.type : "");
}
static value_t ppc_port_attr_machine(struct object *self, const member_t *m) {
    (void)m;
    ppc_port_info_t info;
    return val_str(atalk_ppc_port_info(ppc_obj_slot(self), &info) ? info.machine : "");
}
static value_t ppc_port_attr_node(struct object *self, const member_t *m) {
    (void)m;
    ppc_port_info_t info;
    return val_uint(1, atalk_ppc_port_info(ppc_obj_slot(self), &info) ? info.node : 0);
}
static value_t ppc_port_attr_socket(struct object *self, const member_t *m) {
    (void)m;
    ppc_port_info_t info;
    return val_uint(1, atalk_ppc_port_info(ppc_obj_slot(self), &info) ? info.socket : 0);
}
static value_t ppc_port_attr_auth(struct object *self, const member_t *m) {
    (void)m;
    ppc_port_info_t info;
    return val_bool(atalk_ppc_port_info(ppc_obj_slot(self), &info) ? info.auth_required : false);
}

static const member_t ppc_port_members[] = {
    {.kind = M_ATTR,
     .name = "name",
     .doc = "Port name as the guest's PPC browser shows it",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = ppc_port_attr_name}            },
    {.kind = M_ATTR,
     .name = "type",
     .doc = "Port type string; applications use <signature>ep01",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = ppc_port_attr_type}            },
    {.kind = M_ATTR,
     .name = "machine",
     .doc = "NBP name of the machine holding the port",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = ppc_port_attr_machine}         },
    {.kind = M_ATTR,
     .name = "node",
     .doc = "LLAP node of that machine",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .width = 1, .get = ppc_port_attr_node}  },
    {.kind = M_ATTR,
     .name = "socket",
     .doc = "Its PPC connection-listening socket",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .width = 1, .get = ppc_port_attr_socket}},
    {.kind = M_ATTR,
     .name = "auth_required",
     .doc = "True if the port refuses guest links",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = ppc_port_attr_auth}              },
};

const class_desc_t ppc_port_class = {
    .name = "ppc_port",
    .members = ppc_port_members,
    .n_members = ARRAY_LEN(ppc_port_members),
};

// --- appletalk.ppc.ports -----------------------------------------------------

static struct object *ppc_ports_get(struct object *self, int index) {
    (void)self;
    if (index < 0 || index >= atalk_ppc_port_count())
        return NULL;
    return g_ppc_port_objs[index];
}
static int ppc_ports_count_cb(struct object *self) {
    (void)self;
    return atalk_ppc_port_count();
}
static int ppc_ports_next(struct object *self, int prev) {
    (void)self;
    int next = prev + 1;
    return (next < atalk_ppc_port_count()) ? next : -1;
}
static struct object *ppc_ports_lookup(struct object *self, const char *name) {
    (void)self;
    int idx = atalk_ppc_port_find(name);
    return (idx >= 0) ? g_ppc_port_objs[idx] : NULL;
}
static value_t ppc_ports_attr_count(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, (uint64_t)atalk_ppc_port_count());
}

static const member_t ppc_ports_members[] = {
    {.kind = M_ATTR,
     .name = "count",
     .doc = "Program-linking ports discovered by the last browse",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .width = 4, .get = ppc_ports_attr_count}},
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &ppc_port_class,
               .indexed = true,
               .get = ppc_ports_get,
               .count = ppc_ports_count_cb,
               .next = ppc_ports_next,
               .lookup = ppc_ports_lookup}},
};

const class_desc_t ppc_ports_class = {
    .name = "ppc_ports",
    .members = ppc_ports_members,
    .n_members = ARRAY_LEN(ppc_ports_members),
};

// --- appletalk.ppc.sessions[i] -----------------------------------------------

static ppc_session_t *ppc_obj_session(struct object *self) {
    return atalk_ppc_session_at(ppc_obj_slot(self));
}

static value_t ppc_session_attr_id(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, atalk_ppc_session_id(ppc_obj_session(self)));
}
static value_t ppc_session_attr_state(struct object *self, const member_t *m) {
    (void)m;
    int st = (int)atalk_ppc_session_state(ppc_obj_session(self));
    if (st < 0 || st >= PPC_SESSION_STATE_COUNT)
        st = 0;
    return val_enum(st, PPC_SESSION_STATE_NAMES, PPC_SESSION_STATE_COUNT);
}
static value_t ppc_session_attr_role(struct object *self, const member_t *m) {
    (void)m;
    return val_str(atalk_ppc_session_initiator(ppc_obj_session(self)) ? "initiator" : "responder");
}
static value_t ppc_session_attr_port(struct object *self, const member_t *m) {
    (void)m;
    return val_str(atalk_ppc_session_port(ppc_obj_session(self)));
}
static value_t ppc_session_attr_peer_node(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(1, atalk_ppc_session_peer_node(ppc_obj_session(self)));
}
static value_t ppc_session_attr_bytes_in(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(8, atalk_ppc_session_bytes_in(ppc_obj_session(self)));
}
static value_t ppc_session_attr_bytes_out(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(8, atalk_ppc_session_bytes_out(ppc_obj_session(self)));
}

static const member_t ppc_session_members[] = {
    {.kind = M_ATTR,
     .name = "id",
     .doc = "Stable identity of this session",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .width = 4, .get = ppc_session_attr_id}                               },
    {.kind = M_ATTR,
     .name = "state",
     .doc = "Session state",
     .flags = VAL_RO,
     .attr = {.type = V_ENUM, .enum_values = PPC_SESSION_STATE_NAMES, .get = ppc_session_attr_state}},
    {.kind = M_ATTR,
     .name = "role",
     .doc = "initiator if we asked for the session, else responder",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = ppc_session_attr_role}                                       },
    {.kind = M_ATTR,
     .name = "port",
     .doc = "The port at the far end",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = ppc_session_attr_port}                                       },
    {.kind = M_ATTR,
     .name = "peer_node",
     .doc = "LLAP node of the far end",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .width = 1, .get = ppc_session_attr_peer_node}                        },
    {.kind = M_ATTR,
     .name = "bytes_in",
     .doc = "Session bytes received",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .width = 8, .get = ppc_session_attr_bytes_in}                         },
    {.kind = M_ATTR,
     .name = "bytes_out",
     .doc = "Session bytes sent",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .width = 8, .get = ppc_session_attr_bytes_out}                        },
};

const class_desc_t ppc_session_class = {
    .name = "ppc_session",
    .members = ppc_session_members,
    .n_members = ARRAY_LEN(ppc_session_members),
};

static struct object *ppc_sessions_get(struct object *self, int index) {
    (void)self;
    if (!atalk_ppc_session_at(index))
        return NULL;
    return g_ppc_session_objs[index];
}
static int ppc_sessions_count_cb(struct object *self) {
    (void)self;
    int n = 0;
    for (int i = 0; i < PPC_MAX_SESSIONS; i++)
        if (atalk_ppc_session_at(i))
            n++;
    return n;
}
static int ppc_sessions_next(struct object *self, int prev) {
    (void)self;
    for (int i = prev + 1; i < PPC_MAX_SESSIONS; i++)
        if (atalk_ppc_session_at(i))
            return i;
    return -1;
}
static value_t ppc_sessions_attr_count(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, (uint64_t)ppc_sessions_count_cb(self));
}

static const member_t ppc_sessions_members[] = {
    {.kind = M_ATTR,
     .name = "count",
     .doc = "Live PPC sessions",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .width = 4, .get = ppc_sessions_attr_count}},
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &ppc_session_class,
               .indexed = true,
               .get = ppc_sessions_get,
               .count = ppc_sessions_count_cb,
               .next = ppc_sessions_next}},
};

const class_desc_t ppc_sessions_class = {
    .name = "ppc_sessions",
    .members = ppc_sessions_members,
    .n_members = ARRAY_LEN(ppc_sessions_members),
};

// --- appletalk.ppc.stats -----------------------------------------------------

static value_t ppc_stats_attr(struct object *self, const member_t *m) {
    (void)self;
    const ppc_stats_t *st = atalk_ppc_get_stats();
    size_t offset = (size_t)(uintptr_t)m->attr.user_data;
    return val_uint(8, *(const uint64_t *)((const uint8_t *)st + offset));
}

#define PPC_STAT_MEMBER(field, doc_text)                                                                               \
    {                                                                                                                  \
        .kind = M_ATTR, .name = #field, .doc = doc_text, .flags = VAL_RO, .attr = {                                    \
            .type = V_UINT,                                                                                            \
            .width = 8,                                                                                                \
            .get = ppc_stats_attr,                                                                                     \
            .user_data = (const void *)(uintptr_t)offsetof(ppc_stats_t, field)                                         \
        }                                                                                                              \
    }

static const member_t ppc_stats_members[] = {
    PPC_STAT_MEMBER(sessions_opened, "Sessions that reached the open state"),
    PPC_STAT_MEMBER(sessions_rejected, "Session requests the far side turned down"),
    PPC_STAT_MEMBER(sessions_refused, "Session requests we turned down"),
    PPC_STAT_MEMBER(blocks_in, "Message blocks received"),
    PPC_STAT_MEMBER(blocks_out, "Message blocks sent"),
    PPC_STAT_MEMBER(browses, "Port browses started"),
};

const class_desc_t ppc_stats_class = {
    .name = "ppc_stats",
    .members = ppc_stats_members,
    .n_members = ARRAY_LEN(ppc_stats_members),
};

// --- appletalk.ppc -----------------------------------------------------------

static value_t ppc_method_browse(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    char err[192] = "";
    if (atalk_ppc_browse(err, sizeof(err)) != 0)
        return val_err("cannot browse for program-linking ports: %s", err);
    return val_none();
}

static value_t ppc_attr_browsing(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_bool(atalk_ppc_browse_in_flight());
}

static const member_t ppc_members[] = {
    {.kind = M_ATTR,
     .name = "browsing",
     .doc = "True while a port browse is still waiting on the network",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = ppc_attr_browsing}},
    {.kind = M_METHOD,
     .name = "browse",
     .doc = "Look for program-linking ports on the network; run the scheduler, then read `ports`",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = ppc_method_browse, .ui_flags = MM_MUTATE}},
};

const class_desc_t ppc_class = {
    .name = "ppc",
    .members = ppc_members,
    .n_members = ARRAY_LEN(ppc_members),
};

void atalk_ppc_install_objects(struct object *parent) {
    if (!parent || g_ppc_object)
        return;
    g_ppc_object = object_new(&ppc_class, NULL, "ppc");
    if (!g_ppc_object)
        return;
    object_attach(parent, g_ppc_object);

    g_ppc_ports_object = object_new(&ppc_ports_class, NULL, "ports");
    if (g_ppc_ports_object)
        object_attach(g_ppc_object, g_ppc_ports_object);
    g_ppc_sessions_object = object_new(&ppc_sessions_class, NULL, "sessions");
    if (g_ppc_sessions_object) {
        object_set_category(g_ppc_sessions_object, M_CAT_ADVANCED);
        object_attach(g_ppc_object, g_ppc_sessions_object);
    }
    g_ppc_stats_object = object_new(&ppc_stats_class, NULL, "stats");
    if (g_ppc_stats_object) {
        object_set_category(g_ppc_stats_object, M_CAT_ADVANCED);
        object_attach(g_ppc_object, g_ppc_stats_object);
    }

    for (int i = 0; i < PPC_MAX_PORTS; i++) {
        g_ppc_port_data[i].slot = i;
        g_ppc_port_objs[i] = object_new(&ppc_port_class, &g_ppc_port_data[i], NULL);
    }
    for (int i = 0; i < PPC_MAX_SESSIONS; i++) {
        g_ppc_session_data[i].slot = i;
        g_ppc_session_objs[i] = object_new(&ppc_session_class, &g_ppc_session_data[i], NULL);
    }
}

void atalk_ppc_remove_objects(void) {
    for (int i = 0; i < PPC_MAX_PORTS; i++) {
        if (g_ppc_port_objs[i])
            object_delete(g_ppc_port_objs[i]);
        g_ppc_port_objs[i] = NULL;
    }
    for (int i = 0; i < PPC_MAX_SESSIONS; i++) {
        if (g_ppc_session_objs[i])
            object_delete(g_ppc_session_objs[i]);
        g_ppc_session_objs[i] = NULL;
    }
    struct object **nodes[] = {&g_ppc_ports_object, &g_ppc_sessions_object, &g_ppc_stats_object, &g_ppc_object};
    for (int i = 0; i < ARRAY_LEN(nodes); i++) {
        if (!*nodes[i])
            continue;
        object_detach(*nodes[i]);
        object_delete(*nodes[i]);
        *nodes[i] = NULL;
    }
}
