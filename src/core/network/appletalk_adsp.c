// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_adsp.c
// ADSP — AppleTalk Data Stream Protocol endpoint (DDP type 7).
//
// Reference: Inside AppleTalk, 2nd ed., chapter 12, distilled into
// docs/core/network/appletalk.md §III.3.  Section numbers in the comments
// below are that chapter's own page numbers (12-nn).
//
// The file has three parts:
//   1. the protocol engine — instance-based, transport- and clock-agnostic,
//      so the unit suite can run two endpoints back to back;
//   2. the production instance — one engine wired to the emulator's DDP
//      layer and to the scheduler, so every timer is emulated time;
//   3. the object-model surface `appletalk.adsp`.

// ============================================================================
// Includes
// ============================================================================

#include "appletalk_adsp.h"

#include "appletalk.h"
#include "appletalk_internal.h"
#include "common.h"
#include "log.h"
#include "object.h"
#include "scheduler.h"
#include "value.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("adsp");

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

// ============================================================================
// Constants and Macros
// ============================================================================

#define ADSP_MAX_LISTENERS 4

// Timer slots kept per connection end.  All deadlines are emulated
// nanoseconds; ADSP_NO_DEADLINE disables a slot.
typedef enum {
    ADSP_TIMER_OPEN = 0, // open-request retransmission (12-30)
    ADSP_TIMER_PROBE, // connection timer / probe (12-5)
    ADSP_TIMER_RETRANSMIT, // unacknowledged data (12-7)
    ADSP_TIMER_ATTENTION, // attention retransmission (12-21)
    ADSP_TIMER_FRESET, // forward-reset retransmission (12-10)
    ADSP_TIMER_COUNT,
} adsp_timer_slot_t;

#define ADSP_NO_DEADLINE UINT64_MAX

const char *const ADSP_STATE_NAMES[] = {"closed", "listening", "opening", "established", "open"};

// ============================================================================
// Type Definitions
// ============================================================================

// One connection end: the state descriptor of Inside AppleTalk 12-10 plus the
// queues, timers and bookkeeping an implementation needs around it.
struct adsp_conn {
    bool in_use;
    int slot; // stable index into the stack's table
    int id; // stable identity for the object model
    adsp_state_t state;
    bool initiator; // we sent the first Open Connection Request

    uint8_t local_socket;
    atalk_socket_addr_t remote;
    uint16_t local_cid;
    uint16_t remote_cid;

    // Sequencing variables (12-10).
    uint32_t send_seq;
    uint32_t first_rtmt_seq;
    uint32_t send_wdw_seq;
    uint32_t recv_seq;
    uint16_t recv_wdw;
    uint32_t attn_send_seq;
    uint32_t attn_recv_seq;

    // Send queue.  Slot i carries the stream unit numbered first_rtmt_seq+i:
    // either a data byte (eom[i] == 0) or an end-of-message marker, which
    // consumes a sequence number but carries no byte (12-9).
    uint8_t data[ADSP_SEND_QUEUE];
    uint8_t eom[ADSP_SEND_QUEUE];
    int queued; // units in the queue

    // The single outstanding attention message (12-21).
    bool attn_pending;
    uint16_t attn_code;
    uint8_t attn_data[ADSP_MAX_ATTN_DATA];
    int attn_len;

    bool freset_pending;

    int open_retries;
    int probe_count;
    int oos_run; // consecutive out-of-sequence data packets

    uint64_t deadline[ADSP_TIMER_COUNT];

    uint64_t bytes_in;
    uint64_t bytes_out;
    uint64_t retransmits;

    const adsp_client_t *client;
    void *client_ctx;
};

// A connection-listening socket (12-35).
typedef struct {
    bool in_use;
    uint8_t socket;
    const adsp_client_t *client;
    void *client_ctx;
} adsp_listener_t;

struct adsp_stack {
    adsp_config_t cfg;
    adsp_conn_t conns[ADSP_MAX_CONNECTIONS];
    adsp_listener_t listeners[ADSP_MAX_LISTENERS];
    uint16_t last_cid; // LastConnID (12-6)
    int next_id;
    adsp_stats_t stats;
};

// ============================================================================
// Forward Declarations
// ============================================================================

static void adsp_flush(adsp_stack_t *s, adsp_conn_t *c);
// True while a connection end is still allocated.  Client callbacks may close
// the very end they were called about, so every caller that keeps using `c`
// after a callback must re-check this first.
static bool adsp_conn_live(const adsp_conn_t *c);
static void adsp_arm(adsp_stack_t *s, adsp_conn_t *c, adsp_timer_slot_t slot, uint64_t delay_ns);
static void adsp_disarm(adsp_conn_t *c, adsp_timer_slot_t slot);
static void adsp_conn_release(adsp_stack_t *s, adsp_conn_t *c, const char *reason, bool notify);

// ============================================================================
// Operations — byte order and sequence arithmetic
// ============================================================================

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Sequence numbers wrap at 2^32 (12-7), so ordering is a signed difference.
static bool seq_le(uint32_t a, uint32_t b) {
    return (int32_t)(b - a) >= 0;
}

static bool seq_lt(uint32_t a, uint32_t b) {
    return (int32_t)(b - a) > 0;
}

static int imin(int a, int b) {
    return a < b ? a : b;
}

// ============================================================================
// Operations — packet transmission
// ============================================================================

// Build one ADSP packet (13-byte header plus body) and hand it to the host.
// `body` carries the open-connection parameters, the stream bytes or the
// attention payload depending on the descriptor.
static int adsp_emit(adsp_stack_t *s, const atalk_socket_addr_t *dest, uint8_t src_socket, uint16_t src_cid,
                     uint32_t first_seq, uint32_t next_seq, uint16_t wdw, uint8_t desc, const uint8_t *body,
                     int body_len) {
    uint8_t pkt[ADSP_HEADER_SIZE + ADSP_MAX_DATA];
    if (body_len < 0 || body_len > ADSP_MAX_DATA)
        return -1;

    put16(&pkt[0], src_cid);
    put32(&pkt[2], first_seq);
    put32(&pkt[6], next_seq);
    put16(&pkt[10], wdw);
    pkt[12] = desc;
    if (body_len > 0 && body)
        memcpy(&pkt[ADSP_HEADER_SIZE], body, (size_t)body_len);

    int total = ADSP_HEADER_SIZE + body_len;
    s->stats.packets_out++;
    LOG(7, "ADSP tx -> %u:%u sock=%u cid=0x%04X desc=0x%02X first=%u next=%u wdw=%u len=%d", (unsigned)dest->node,
        (unsigned)dest->socket, (unsigned)src_socket, (unsigned)src_cid, (unsigned)desc, (unsigned)first_seq,
        (unsigned)next_seq, (unsigned)wdw, body_len);
    if (!s->cfg.send)
        return -1;
    return s->cfg.send(s->cfg.ctx, dest, src_socket, pkt, total);
}

// Send a control packet on an established (or opening) connection end.
static int adsp_send_control(adsp_stack_t *s, adsp_conn_t *c, uint8_t code, bool ack_req) {
    uint8_t desc = (uint8_t)(ADSP_DESC_CONTROL | (code & ADSP_DESC_CODE_MASK));
    if (ack_req)
        desc |= ADSP_DESC_ACK_REQ;
    return adsp_emit(s, &c->remote, c->local_socket, c->local_cid, c->send_seq, c->recv_seq, c->recv_wdw, desc, NULL,
                     0);
}

// Send an open-dialog control packet: the 8 open-connection parameters follow
// the header (12-27).
static int adsp_send_open(adsp_stack_t *s, adsp_conn_t *c, uint8_t code, uint16_t dest_cid) {
    uint8_t params[ADSP_OPEN_PARAMS_SIZE];
    put16(&params[0], ADSP_VERSION);
    put16(&params[2], dest_cid);
    put32(&params[4], c->attn_recv_seq);
    uint8_t desc = (uint8_t)(ADSP_DESC_CONTROL | (code & ADSP_DESC_CODE_MASK));
    return adsp_emit(s, &c->remote, c->local_socket, c->local_cid, c->send_seq, c->recv_seq, c->recv_wdw, desc, params,
                     sizeof(params));
}

// Deny an open request we cannot honour.  The denial carries source ConnID 0
// and the requester's ConnID in the destination field (12-27).
static void adsp_send_denial(adsp_stack_t *s, const atalk_socket_addr_t *to, uint8_t local_socket, uint16_t dest_cid) {
    uint8_t params[ADSP_OPEN_PARAMS_SIZE];
    put16(&params[0], ADSP_VERSION);
    put16(&params[2], dest_cid);
    put32(&params[4], 0);
    s->stats.open_denials++;
    adsp_emit(s, to, local_socket, 0, 0, 0, 0, (uint8_t)(ADSP_DESC_CONTROL | ADSP_CTL_OPEN_DENY), params,
              sizeof(params));
}

// ============================================================================
// Operations — connection table
// ============================================================================

static adsp_conn_t *adsp_alloc_conn(adsp_stack_t *s) {
    for (int i = 0; i < ADSP_MAX_CONNECTIONS; i++) {
        adsp_conn_t *c = &s->conns[i];
        if (c->in_use)
            continue;
        memset(c, 0, sizeof(*c));
        c->in_use = true;
        c->slot = i;
        c->id = s->next_id++;
        c->recv_wdw = ADSP_RECV_WINDOW;
        for (int t = 0; t < ADSP_TIMER_COUNT; t++)
            c->deadline[t] = ADSP_NO_DEADLINE;
        return c;
    }
    LOG(2, "ADSP: connection table full");
    return NULL;
}

// A locally unique, non-zero ConnID (12-6).  LastConnID walks forward rather
// than starting from a random value: identical scripts must produce identical
// wire traces.
static uint16_t adsp_next_cid(adsp_stack_t *s, uint8_t socket) {
    for (int attempt = 0; attempt <= 0xFFFF; attempt++) {
        s->last_cid = (uint16_t)(s->last_cid == 0xFFFF ? 1 : s->last_cid + 1);
        bool taken = false;
        for (int i = 0; i < ADSP_MAX_CONNECTIONS; i++) {
            const adsp_conn_t *c = &s->conns[i];
            if (c->in_use && c->local_socket == socket && c->local_cid == s->last_cid) {
                taken = true;
                break;
            }
        }
        if (!taken)
            return s->last_cid;
    }
    return 1;
}

static bool adsp_addr_eq(const atalk_socket_addr_t *a, const atalk_socket_addr_t *b) {
    return a->node == b->node && a->socket == b->socket && a->net == b->net;
}

// Match an incoming packet to a connection end: same local socket, same
// remote internet socket address, same remote ConnID (12-6).
static adsp_conn_t *adsp_find_conn(adsp_stack_t *s, const atalk_socket_addr_t *from, uint8_t dst_socket,
                                   uint16_t src_cid) {
    for (int i = 0; i < ADSP_MAX_CONNECTIONS; i++) {
        adsp_conn_t *c = &s->conns[i];
        if (!c->in_use || c->state == ADSP_STATE_LISTENING)
            continue;
        if (c->local_socket != dst_socket || !adsp_addr_eq(&c->remote, from))
            continue;
        if (c->remote_cid != 0 && c->remote_cid != src_cid)
            continue;
        return c;
    }
    return NULL;
}

// Find the end an open acknowledgment or denial refers to: the destination
// ConnID of the open parameters is our own LocConnID (12-27).
static adsp_conn_t *adsp_find_by_local_cid(adsp_stack_t *s, uint8_t dst_socket, uint16_t local_cid) {
    if (local_cid == 0)
        return NULL;
    for (int i = 0; i < ADSP_MAX_CONNECTIONS; i++) {
        adsp_conn_t *c = &s->conns[i];
        if (c->in_use && c->state != ADSP_STATE_LISTENING && c->local_socket == dst_socket && c->local_cid == local_cid)
            return c;
    }
    return NULL;
}

static adsp_listener_t *adsp_find_listener(adsp_stack_t *s, uint8_t socket) {
    for (int i = 0; i < ADSP_MAX_LISTENERS; i++) {
        if (s->listeners[i].in_use && s->listeners[i].socket == socket)
            return &s->listeners[i];
    }
    return NULL;
}

// ============================================================================
// Operations — timers
// ============================================================================

static bool adsp_conn_live(const adsp_conn_t *c) {
    return c && c->in_use;
}

static uint64_t adsp_now(adsp_stack_t *s) {
    return s->cfg.now_ns ? s->cfg.now_ns(s->cfg.ctx) : 0;
}

// Tell the host when we next need to be called, so it can arm one event.
static void adsp_rearm_host(adsp_stack_t *s) {
    if (s->cfg.rearm)
        s->cfg.rearm(s->cfg.ctx, adsp_next_deadline(s));
}

static void adsp_arm(adsp_stack_t *s, adsp_conn_t *c, adsp_timer_slot_t slot, uint64_t delay_ns) {
    c->deadline[slot] = adsp_now(s) + delay_ns;
}

static void adsp_disarm(adsp_conn_t *c, adsp_timer_slot_t slot) {
    c->deadline[slot] = ADSP_NO_DEADLINE;
}

uint64_t adsp_next_deadline(adsp_stack_t *s) {
    uint64_t best = ADSP_NO_DEADLINE;
    if (!s)
        return best;
    for (int i = 0; i < ADSP_MAX_CONNECTIONS; i++) {
        const adsp_conn_t *c = &s->conns[i];
        if (!c->in_use)
            continue;
        for (int t = 0; t < ADSP_TIMER_COUNT; t++)
            if (c->deadline[t] < best)
                best = c->deadline[t];
    }
    return best;
}

// Restart the connection timer: any packet from the remote end resets it (12-5).
static void adsp_touch(adsp_stack_t *s, adsp_conn_t *c) {
    c->probe_count = 0;
    adsp_arm(s, c, ADSP_TIMER_PROBE, ADSP_PROBE_INTERVAL_NS);
}

// ============================================================================
// Operations — the send stream
// ============================================================================

// Units the stream holds beyond the last byte we have transmitted.
static uint32_t adsp_queue_end(const adsp_conn_t *c) {
    return c->first_rtmt_seq + (uint32_t)c->queued;
}

// Drop acknowledged units off the front of the send queue (12-7).
static void adsp_consume_acked(adsp_conn_t *c, uint32_t next_recv_seq) {
    int drop = (int)(next_recv_seq - c->first_rtmt_seq);
    if (drop <= 0)
        return;
    if (drop > c->queued)
        drop = c->queued;
    memmove(c->data, c->data + drop, (size_t)(c->queued - drop));
    memmove(c->eom, c->eom + drop, (size_t)(c->queued - drop));
    c->queued -= drop;
    c->first_rtmt_seq = next_recv_seq;
}

// Apply the acknowledgment carried by every packet from the remote end (12-7,
// 12-8): advance the send queue and the send window, never backwards.
static void adsp_update_send_window(adsp_stack_t *s, adsp_conn_t *c, uint32_t pkt_next, uint16_t pkt_wdw) {
    if (seq_le(c->first_rtmt_seq, pkt_next) && seq_le(pkt_next, c->send_seq)) {
        adsp_consume_acked(c, pkt_next);
        if (c->first_rtmt_seq == c->send_seq)
            adsp_disarm(c, ADSP_TIMER_RETRANSMIT); // nothing outstanding
        else
            adsp_arm(s, c, ADSP_TIMER_RETRANSMIT, ADSP_RETRANSMIT_NS);
    }
    // SendWdwSeq never decreases: the client cannot revoke buffer space (12-8).
    uint32_t candidate = pkt_next + pkt_wdw - 1;
    if (seq_lt(c->send_wdw_seq, candidate))
        c->send_wdw_seq = candidate;
}

// Transmit everything the window allows, splitting at end-of-message markers
// and at ADSP_MAX_DATA.  The last packet of a burst requests an acknowledgment
// so a stalled peer is nudged rather than waited on (Figure 12-3).
static void adsp_flush(adsp_stack_t *s, adsp_conn_t *c) {
    if (c->state != ADSP_STATE_OPEN)
        return;
    uint32_t end = adsp_queue_end(c);
    while (seq_lt(c->send_seq, end)) {
        if (!seq_le(c->send_seq, c->send_wdw_seq))
            break; // the peer has no room for the next byte
        int off = (int)(c->send_seq - c->first_rtmt_seq);
        int avail = (int)(end - c->send_seq);
        int window = (int)(c->send_wdw_seq - c->send_seq + 1);
        int limit = imin(imin(avail, window), ADSP_MAX_DATA);

        // A marker ends the packet: the bytes before it travel with EOM set.
        int payload = limit;
        bool eom = false;
        for (int i = 0; i < limit; i++) {
            if (c->eom[off + i]) {
                payload = i;
                eom = true;
                break;
            }
        }
        // An EOM marker at the head of the window is a data-less EOM packet (12-9).
        int consumed = eom ? payload + 1 : payload;
        if (consumed == 0)
            break;

        bool last = (uint32_t)(c->send_seq + consumed) == end;
        uint8_t desc = 0;
        if (eom)
            desc |= ADSP_DESC_EOM;
        if (last)
            desc |= ADSP_DESC_ACK_REQ;
        adsp_emit(s, &c->remote, c->local_socket, c->local_cid, c->send_seq, c->recv_seq, c->recv_wdw, desc,
                  &c->data[off], payload);
        c->send_seq += (uint32_t)consumed;
        c->bytes_out += (uint64_t)payload;
        s->stats.bytes_out += (uint64_t)payload;
    }
    if (seq_lt(c->first_rtmt_seq, c->send_seq))
        adsp_arm(s, c, ADSP_TIMER_RETRANSMIT, ADSP_RETRANSMIT_NS);
}

// Put the outstanding attention message back on the wire (12-21).  Attention
// packets reuse the header's sequence fields for the attention sub-channel.
static void adsp_emit_attention(adsp_stack_t *s, adsp_conn_t *c) {
    uint8_t body[2 + ADSP_MAX_ATTN_DATA];
    put16(&body[0], c->attn_code);
    if (c->attn_len > 0)
        memcpy(&body[2], c->attn_data, (size_t)c->attn_len);
    uint8_t desc = (uint8_t)(ADSP_DESC_ATTENTION | ADSP_DESC_ACK_REQ);
    adsp_emit(s, &c->remote, c->local_socket, c->local_cid, c->attn_send_seq, c->attn_recv_seq, 0, desc, body,
              2 + c->attn_len);
    adsp_arm(s, c, ADSP_TIMER_ATTENTION, ADSP_ATTN_RETRY_NS);
}

// ============================================================================
// Operations — inbound packet handling
// ============================================================================

// Adopt the remote end's parameters from an open-connection packet (12-23).
static void adsp_establish(adsp_conn_t *c, uint16_t remote_cid, uint32_t pkt_next, uint16_t pkt_wdw,
                           uint32_t attn_recv) {
    c->remote_cid = remote_cid;
    c->send_seq = pkt_next;
    c->first_rtmt_seq = pkt_next;
    c->send_wdw_seq = pkt_next + pkt_wdw - 1;
    c->attn_send_seq = attn_recv;
}

static void adsp_go_open(adsp_stack_t *s, adsp_conn_t *c) {
    c->state = ADSP_STATE_OPEN;
    adsp_disarm(c, ADSP_TIMER_OPEN);
    adsp_touch(s, c);
    s->stats.opens++;
    LOG(4, "ADSP: connection %d open (sock=%u peer=%u:%u cid=0x%04X/0x%04X)", c->id, (unsigned)c->local_socket,
        (unsigned)c->remote.node, (unsigned)c->remote.socket, (unsigned)c->local_cid, (unsigned)c->remote_cid);
    if (c->client && c->client->on_open)
        c->client->on_open(c->client_ctx, c);
    if (!adsp_conn_live(c))
        return; // the client closed it from inside on_open
    adsp_flush(s, c);
}

// The connection-opening dialog (12-22 … 12-34).
static void adsp_handle_open_dialog(adsp_stack_t *s, const atalk_socket_addr_t *from, uint8_t dst_socket,
                                    uint16_t src_cid, uint32_t pkt_first, uint32_t pkt_next, uint16_t pkt_wdw,
                                    uint8_t code, const uint8_t *body, int body_len) {
    if (body_len < ADSP_OPEN_PARAMS_SIZE) {
        LOG(3, "ADSP: open packet without open-connection parameters (len=%d)", body_len);
        return;
    }
    uint16_t version = get16(&body[0]);
    uint16_t dest_cid = get16(&body[2]);
    uint32_t attn_recv = get32(&body[4]);

    switch (code) {
    case ADSP_CTL_OPEN_REQ: {
        // An incompatible version must be denied (12-27).
        if (version != ADSP_VERSION) {
            LOG(2, "ADSP: denying open request with version 0x%04X", (unsigned)version);
            adsp_send_denial(s, from, dst_socket, src_cid);
            return;
        }
        adsp_conn_t *c = adsp_find_conn(s, from, dst_socket, src_cid);
        if (c && c->remote_cid == src_cid) {
            // A duplicate request still gets the acknowledgment (12-30).
            if (c->state == ADSP_STATE_ESTABLISHED) {
                adsp_send_open(s, c, ADSP_CTL_OPEN_REQ_ACK, c->remote_cid);
            } else if (c->state == ADSP_STATE_OPEN) {
                // Only a request that has seen none of our data can be a
                // retransmission; anything else is a late duplicate (12-33).
                if (pkt_first == c->recv_seq) {
                    adsp_send_open(s, c, ADSP_CTL_OPEN_ACK, c->remote_cid);
                    c->send_seq = c->first_rtmt_seq; // retransmit what we sent
                    adsp_flush(s, c);
                } else {
                    LOG(6, "ADSP: discarding late duplicate open request on connection %d", c->id);
                }
            }
            return;
        }
        // Both ends opened at once: we already have a request outstanding to
        // this remote, so adopt its parameters and acknowledge (12-25).
        for (int i = 0; i < ADSP_MAX_CONNECTIONS; i++) {
            adsp_conn_t *o = &s->conns[i];
            if (o->in_use && o->state == ADSP_STATE_OPENING && o->local_socket == dst_socket &&
                adsp_addr_eq(&o->remote, from)) {
                adsp_establish(o, src_cid, pkt_next, pkt_wdw, attn_recv);
                adsp_send_open(s, o, ADSP_CTL_OPEN_ACK, src_cid);
                adsp_go_open(s, o);
                return;
            }
        }
        adsp_listener_t *l = adsp_find_listener(s, dst_socket);
        if (!l) {
            LOG(4, "ADSP: open request for socket %u with no listener", (unsigned)dst_socket);
            adsp_send_denial(s, from, dst_socket, src_cid);
            return;
        }
        if (l->client && l->client->on_accept && !l->client->on_accept(l->client_ctx, from)) {
            LOG(3, "ADSP: listener on socket %u refused %u:%u", (unsigned)dst_socket, (unsigned)from->node,
                (unsigned)from->socket);
            adsp_send_denial(s, from, dst_socket, src_cid);
            return;
        }
        adsp_conn_t *n = adsp_alloc_conn(s);
        if (!n) {
            adsp_send_denial(s, from, dst_socket, src_cid);
            return;
        }
        n->state = ADSP_STATE_ESTABLISHED;
        n->initiator = false;
        n->local_socket = dst_socket;
        n->remote = *from;
        n->local_cid = adsp_next_cid(s, dst_socket);
        n->client = l->client;
        n->client_ctx = l->client_ctx;
        adsp_establish(n, src_cid, pkt_next, pkt_wdw, attn_recv);
        adsp_touch(s, n);
        LOG(4, "ADSP: accepted open request from %u:%u on socket %u (connection %d)", (unsigned)from->node,
            (unsigned)from->socket, (unsigned)dst_socket, n->id);
        adsp_send_open(s, n, ADSP_CTL_OPEN_REQ_ACK, src_cid);
        return;
    }
    case ADSP_CTL_OPEN_ACK: {
        adsp_conn_t *c = adsp_find_by_local_cid(s, dst_socket, dest_cid);
        if (!c)
            return;
        adsp_touch(s, c);
        if (c->state == ADSP_STATE_ESTABLISHED)
            adsp_go_open(s, c);
        return;
    }
    case ADSP_CTL_OPEN_REQ_ACK: {
        adsp_conn_t *c = adsp_find_by_local_cid(s, dst_socket, dest_cid);
        if (!c || c->state != ADSP_STATE_OPENING)
            return;
        if (version != ADSP_VERSION) {
            adsp_conn_release(s, c, "peer speaks an incompatible ADSP version", true);
            return;
        }
        adsp_establish(c, src_cid, pkt_next, pkt_wdw, attn_recv);
        adsp_send_open(s, c, ADSP_CTL_OPEN_ACK, src_cid);
        adsp_go_open(s, c);
        return;
    }
    case ADSP_CTL_OPEN_DENY: {
        adsp_conn_t *c = adsp_find_by_local_cid(s, dst_socket, dest_cid);
        s->stats.open_denials++;
        if (c && c->state == ADSP_STATE_OPENING)
            adsp_conn_release(s, c, "the remote end denied the open request", true);
        return;
    }
    default:
        break;
    }
}

// An attention message or its acknowledgment (12-19).
static void adsp_handle_attention(adsp_stack_t *s, adsp_conn_t *c, uint8_t desc, uint32_t pkt_attn_send,
                                  uint32_t pkt_attn_recv, const uint8_t *body, int body_len) {
    if ((desc & ADSP_DESC_CODE_MASK) != 0) {
        LOG(3, "ADSP: attention packet with control code %u discarded", (unsigned)(desc & ADSP_DESC_CODE_MASK));
        return;
    }
    // Piggybacked acknowledgment: our outstanding message is done once the
    // peer's AttnRecvSeq has moved past it (12-21).
    if (c->attn_pending && pkt_attn_recv == c->attn_send_seq + 1) {
        c->attn_send_seq = pkt_attn_recv;
        c->attn_pending = false;
        adsp_disarm(c, ADSP_TIMER_ATTENTION);
    }
    if (desc & ADSP_DESC_CONTROL)
        return; // an attention-control packet carries no client message

    if (body_len < 2) {
        LOG(3, "ADSP: attention packet shorter than its code field");
        return;
    }
    if (pkt_attn_send != c->attn_recv_seq) {
        LOG(5, "ADSP: out-of-sequence attention (%u, expected %u)", (unsigned)pkt_attn_send,
            (unsigned)c->attn_recv_seq);
        return;
    }
    uint16_t code = get16(&body[0]);
    c->attn_recv_seq++;
    s->stats.attentions_in++;
    // Acknowledge with an attention-control packet (no ack request, 12-12).
    adsp_emit(s, &c->remote, c->local_socket, c->local_cid, c->attn_send_seq, c->attn_recv_seq, 0,
              (uint8_t)(ADSP_DESC_CONTROL | ADSP_DESC_ATTENTION), NULL, 0);
    if (c->client && c->client->on_attention)
        c->client->on_attention(c->client_ctx, c, code, body + 2, body_len - 2);
}

// In-order data acceptance (12-7).  Out-of-window buffering is the optional
// in-window variant and is deliberately not implemented.
static void adsp_handle_data(adsp_stack_t *s, adsp_conn_t *c, uint8_t desc, uint32_t pkt_first, const uint8_t *body,
                             int body_len) {
    bool eom = (desc & ADSP_DESC_EOM) != 0;
    bool ack_req = (desc & ADSP_DESC_ACK_REQ) != 0;

    if (pkt_first != c->recv_seq) {
        s->stats.out_of_sequence++;
        c->oos_run++;
        LOG(5, "ADSP: out-of-sequence data on connection %d (first=%u expected=%u)", c->id, (unsigned)pkt_first,
            (unsigned)c->recv_seq);
        if (ack_req) {
            adsp_send_control(s, c, ADSP_CTL_PROBE_ACK, false);
        } else if (c->oos_run >= ADSP_OOS_ADVICE_THRESHOLD) {
            adsp_send_control(s, c, ADSP_CTL_RETRANSMIT, false);
            c->oos_run = 0;
        }
        return;
    }

    c->oos_run = 0;
    if (body_len > (int)c->recv_wdw) {
        // Data beyond the advertised window is discarded (12-8).
        LOG(3, "ADSP: discarding %d bytes past the receive window on connection %d", body_len, c->id);
        return;
    }
    c->recv_seq += (uint32_t)body_len;
    if (eom)
        c->recv_seq++; // the marker consumes a sequence number (12-9)
    c->bytes_in += (uint64_t)body_len;
    s->stats.bytes_in += (uint64_t)body_len;
    if ((body_len > 0 || eom) && c->client && c->client->on_data) {
        c->client->on_data(c->client_ctx, c, body, body_len, eom);
        // The client is allowed to close the connection from inside its own
        // callback — the Apple event layer does exactly that when a reply
        // settles an event.  Anything below would be touching a freed end.
        if (!adsp_conn_live(c))
            return;
    }
    // We deliver synchronously, so the window is open again immediately; tell
    // the sender rather than making it wait for its retransmit timer.
    if (ack_req || body_len > 0 || eom)
        adsp_send_control(s, c, ADSP_CTL_PROBE_ACK, false);
}

void adsp_input(adsp_stack_t *s, const atalk_socket_addr_t *from, uint8_t dst_socket, const uint8_t *pkt, int len) {
    if (!s || !from || !pkt)
        return;
    s->stats.packets_in++;
    if (len < ADSP_HEADER_SIZE) {
        LOG(3, "ADSP: runt packet (%d bytes)", len);
        return;
    }
    uint16_t src_cid = get16(&pkt[0]);
    uint32_t pkt_first = get32(&pkt[2]);
    uint32_t pkt_next = get32(&pkt[6]);
    uint16_t pkt_wdw = get16(&pkt[10]);
    uint8_t desc = pkt[12];
    const uint8_t *body = pkt + ADSP_HEADER_SIZE;
    int body_len = len - ADSP_HEADER_SIZE;

    bool control = (desc & ADSP_DESC_CONTROL) != 0;
    bool attention = (desc & ADSP_DESC_ATTENTION) != 0;
    uint8_t code = desc & ADSP_DESC_CODE_MASK;

    LOG(7, "ADSP rx <- %u:%u sock=%u cid=0x%04X desc=0x%02X first=%u next=%u wdw=%u len=%d", (unsigned)from->node,
        (unsigned)from->socket, (unsigned)dst_socket, (unsigned)src_cid, (unsigned)desc, (unsigned)pkt_first,
        (unsigned)pkt_next, (unsigned)pkt_wdw, body_len);

    if (control && !attention && code >= ADSP_CTL_OPEN_REQ && code <= ADSP_CTL_OPEN_DENY) {
        adsp_handle_open_dialog(s, from, dst_socket, src_cid, pkt_first, pkt_next, pkt_wdw, code, body, body_len);
        adsp_rearm_host(s);
        return;
    }
    if (control && code > ADSP_CTL_RETRANSMIT) {
        LOG(3, "ADSP: reserved control code %u rejected", (unsigned)code); // 12-14
        return;
    }

    adsp_conn_t *c = adsp_find_conn(s, from, dst_socket, src_cid);
    if (!c) {
        LOG(5, "ADSP: packet for unknown connection (sock=%u cid=0x%04X)", (unsigned)dst_socket, (unsigned)src_cid);
        return;
    }
    adsp_touch(s, c);

    if (attention) {
        adsp_handle_attention(s, c, desc, pkt_first, pkt_next, body, body_len);
        adsp_rearm_host(s);
        return;
    }
    if (!adsp_conn_live(c)) {
        adsp_rearm_host(s);
        return;
    }

    adsp_update_send_window(s, c, pkt_next, pkt_wdw);

    if (!control) {
        if (c->state != ADSP_STATE_OPEN) {
            // Data before the dialog completes is discarded (12-32).
            LOG(5, "ADSP: data on connection %d before it is open", c->id);
        } else {
            adsp_handle_data(s, c, desc, pkt_first, body, body_len);
        }
        if (!adsp_conn_live(c)) {
            adsp_rearm_host(s);
            return;
        }
        adsp_flush(s, c);
        adsp_rearm_host(s);
        return;
    }

    switch (code) {
    case ADSP_CTL_PROBE_ACK:
        // A probe (ack requested) must be answered immediately (12-14).
        if (desc & ADSP_DESC_ACK_REQ)
            adsp_send_control(s, c, ADSP_CTL_PROBE_ACK, false);
        break;
    case ADSP_CTL_CLOSE:
        adsp_conn_release(s, c, "the remote end closed the connection", true);
        adsp_rearm_host(s);
        return;
    case ADSP_CTL_FWD_RESET:
        // Resynchronise the receive stream, then acknowledge — the reply goes
        // back even when the request was out of range (12-10).
        if (seq_le(c->recv_seq, pkt_first) && seq_le(pkt_first, c->recv_seq + c->recv_wdw)) {
            c->recv_seq = pkt_first;
            s->stats.forward_resets++;
        }
        adsp_send_control(s, c, ADSP_CTL_FWD_RESET_ACK, false);
        break;
    case ADSP_CTL_FWD_RESET_ACK:
        if (c->freset_pending && seq_le(c->send_seq, pkt_next) && seq_le(pkt_next, c->send_wdw_seq + 1)) {
            c->freset_pending = false;
            adsp_disarm(c, ADSP_TIMER_FRESET);
        }
        break;
    case ADSP_CTL_RETRANSMIT:
        // Resend from the byte the peer is missing (12-14).
        c->send_seq = c->first_rtmt_seq;
        c->retransmits++;
        s->stats.retransmits++;
        break;
    default:
        break;
    }
    adsp_flush(s, c);
    adsp_rearm_host(s);
}

// ============================================================================
// Operations — timer expiry
// ============================================================================

static void adsp_fire_open(adsp_stack_t *s, adsp_conn_t *c) {
    if (c->open_retries >= ADSP_OPEN_RETRY_LIMIT) {
        adsp_conn_release(s, c, "the remote end never answered the open request", true);
        return;
    }
    c->open_retries++;
    adsp_send_open(s, c, ADSP_CTL_OPEN_REQ, 0);
    adsp_arm(s, c, ADSP_TIMER_OPEN, ADSP_OPEN_RETRY_NS);
}

static void adsp_fire_probe(adsp_stack_t *s, adsp_conn_t *c) {
    c->probe_count++;
    if (c->probe_count >= ADSP_PROBE_LIMIT) {
        // Four expiries without a packet: the connection is half-open (12-5).
        s->stats.timeouts++;
        adsp_conn_release(s, c, "the connection timer expired (peer unreachable)", true);
        return;
    }
    adsp_send_control(s, c, ADSP_CTL_PROBE_ACK, true);
    adsp_arm(s, c, ADSP_TIMER_PROBE, ADSP_PROBE_INTERVAL_NS);
}

static void adsp_fire_retransmit(adsp_stack_t *s, adsp_conn_t *c) {
    if (!seq_lt(c->first_rtmt_seq, c->send_seq)) {
        adsp_disarm(c, ADSP_TIMER_RETRANSMIT);
        return;
    }
    c->retransmits++;
    s->stats.retransmits++;
    LOG(5, "ADSP: retransmitting from %u on connection %d", (unsigned)c->first_rtmt_seq, c->id);
    c->send_seq = c->first_rtmt_seq;
    adsp_flush(s, c);
    adsp_arm(s, c, ADSP_TIMER_RETRANSMIT, ADSP_RETRANSMIT_NS);
}

void adsp_run_timers(adsp_stack_t *s) {
    if (!s)
        return;
    uint64_t now = adsp_now(s);
    for (int i = 0; i < ADSP_MAX_CONNECTIONS; i++) {
        adsp_conn_t *c = &s->conns[i];
        if (!c->in_use)
            continue;
        for (int t = 0; t < ADSP_TIMER_COUNT && c->in_use; t++) {
            if (c->deadline[t] > now)
                continue;
            adsp_disarm(c, (adsp_timer_slot_t)t);
            switch ((adsp_timer_slot_t)t) {
            case ADSP_TIMER_OPEN:
                adsp_fire_open(s, c);
                break;
            case ADSP_TIMER_PROBE:
                adsp_fire_probe(s, c);
                break;
            case ADSP_TIMER_RETRANSMIT:
                adsp_fire_retransmit(s, c);
                break;
            case ADSP_TIMER_ATTENTION:
                if (c->attn_pending)
                    adsp_emit_attention(s, c);
                break;
            case ADSP_TIMER_FRESET:
                if (c->freset_pending) {
                    adsp_send_control(s, c, ADSP_CTL_FWD_RESET, false);
                    adsp_arm(s, c, ADSP_TIMER_FRESET, ADSP_FRESET_RETRY_NS);
                }
                break;
            case ADSP_TIMER_COUNT:
                break;
            }
        }
    }
    adsp_rearm_host(s);
}

// ============================================================================
// Operations — client API
// ============================================================================

adsp_stack_t *adsp_stack_new(const adsp_config_t *cfg) {
    if (!cfg || !cfg->send)
        return NULL;
    adsp_stack_t *s = (adsp_stack_t *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->cfg = *cfg;
    s->next_id = 1;
    s->last_cid = 0;
    return s;
}

void adsp_stack_free(adsp_stack_t *s) {
    free(s);
}

static void adsp_conn_release(adsp_stack_t *s, adsp_conn_t *c, const char *reason, bool notify) {
    if (!c || !c->in_use)
        return;
    const adsp_client_t *client = c->client;
    void *ctx = c->client_ctx;
    LOG(4, "ADSP: connection %d closed — %s", c->id, reason ? reason : "");
    if (notify && client && client->on_close)
        client->on_close(ctx, c, reason ? reason : "closed");
    memset(c, 0, sizeof(*c));
    for (int t = 0; t < ADSP_TIMER_COUNT; t++)
        c->deadline[t] = ADSP_NO_DEADLINE;
    (void)s;
}

void adsp_close(adsp_stack_t *s, adsp_conn_t *c, const char *reason) {
    if (!s || !c || !c->in_use)
        return;
    if (c->state == ADSP_STATE_OPEN || c->state == ADSP_STATE_ESTABLISHED)
        adsp_send_control(s, c, ADSP_CTL_CLOSE, false); // advisory only (12-38)
    adsp_conn_release(s, c, reason ? reason : "closed locally", true);
    adsp_rearm_host(s);
}

void adsp_close_all(adsp_stack_t *s, const char *reason) {
    if (!s)
        return;
    for (int i = 0; i < ADSP_MAX_CONNECTIONS; i++) {
        adsp_conn_t *c = &s->conns[i];
        if (c->in_use && c->state != ADSP_STATE_LISTENING)
            adsp_close(s, c, reason);
    }
}

int adsp_listen(adsp_stack_t *s, uint8_t socket, const adsp_client_t *client, void *client_ctx) {
    if (!s || socket == 0)
        return -1;
    if (adsp_find_listener(s, socket))
        return -1;
    for (int i = 0; i < ADSP_MAX_LISTENERS; i++) {
        if (s->listeners[i].in_use)
            continue;
        s->listeners[i].in_use = true;
        s->listeners[i].socket = socket;
        s->listeners[i].client = client;
        s->listeners[i].client_ctx = client_ctx;
        LOG(3, "ADSP: listening on socket %u", (unsigned)socket);
        return 0;
    }
    return -1;
}

int adsp_unlisten(adsp_stack_t *s, uint8_t socket) {
    adsp_listener_t *l = s ? adsp_find_listener(s, socket) : NULL;
    if (!l)
        return -1;
    l->in_use = false;
    return 0;
}

adsp_conn_t *adsp_open(adsp_stack_t *s, const atalk_socket_addr_t *dest, uint8_t local_socket,
                       const adsp_client_t *client, void *client_ctx) {
    if (!s || !dest)
        return NULL;
    adsp_conn_t *c = adsp_alloc_conn(s);
    if (!c)
        return NULL;
    c->state = ADSP_STATE_OPENING;
    c->initiator = true;
    c->local_socket = local_socket;
    c->remote = *dest;
    c->local_cid = adsp_next_cid(s, local_socket);
    c->client = client;
    c->client_ctx = client_ctx;
    LOG(4, "ADSP: opening connection %d from socket %u to %u:%u (cid=0x%04X)", c->id, (unsigned)local_socket,
        (unsigned)dest->node, (unsigned)dest->socket, (unsigned)c->local_cid);
    adsp_send_open(s, c, ADSP_CTL_OPEN_REQ, 0);
    c->open_retries = 1;
    adsp_arm(s, c, ADSP_TIMER_OPEN, ADSP_OPEN_RETRY_NS);
    adsp_rearm_host(s);
    return c;
}

int adsp_write(adsp_stack_t *s, adsp_conn_t *c, const uint8_t *data, int len, bool eom) {
    if (!s || !c || !c->in_use || len < 0)
        return -1;
    if (c->state != ADSP_STATE_OPEN && c->state != ADSP_STATE_ESTABLISHED)
        return -1;
    int units = len + (eom ? 1 : 0);
    if (c->queued + units > ADSP_SEND_QUEUE)
        return -1; // the caller must drain before queueing more
    if (len > 0 && data) {
        memcpy(&c->data[c->queued], data, (size_t)len);
        memset(&c->eom[c->queued], 0, (size_t)len);
        c->queued += len;
    }
    if (eom) {
        c->data[c->queued] = 0;
        c->eom[c->queued] = 1;
        c->queued++;
    }
    adsp_flush(s, c);
    adsp_rearm_host(s);
    return len;
}

int adsp_send_attention(adsp_stack_t *s, adsp_conn_t *c, uint16_t code, const uint8_t *data, int len) {
    if (!s || !c || !c->in_use || c->state != ADSP_STATE_OPEN)
        return -1;
    if (len < 0 || len > ADSP_MAX_ATTN_DATA)
        return -1;
    if (c->attn_pending)
        return -1; // only one attention message may be outstanding (12-21)
    if (code >= 0xF000)
        return -1; // $F000..$FFFF are reserved by ADSP (12-19)
    c->attn_code = code;
    c->attn_len = len;
    if (len > 0 && data)
        memcpy(c->attn_data, data, (size_t)len);
    c->attn_pending = true;
    s->stats.attentions_out++;
    adsp_emit_attention(s, c);
    adsp_rearm_host(s);
    return 0;
}

int adsp_forward_reset(adsp_stack_t *s, adsp_conn_t *c) {
    if (!s || !c || !c->in_use || c->state != ADSP_STATE_OPEN)
        return -1;
    // Flush everything unsent and unacknowledged, then resynchronise (12-10).
    c->queued = 0;
    c->first_rtmt_seq = c->send_seq;
    c->freset_pending = true;
    s->stats.forward_resets++;
    adsp_send_control(s, c, ADSP_CTL_FWD_RESET, false);
    adsp_arm(s, c, ADSP_TIMER_FRESET, ADSP_FRESET_RETRY_NS);
    adsp_disarm(c, ADSP_TIMER_RETRANSMIT);
    adsp_rearm_host(s);
    return 0;
}

// ============================================================================
// Operations — accessors
// ============================================================================

int adsp_conn_id(const adsp_conn_t *c) {
    return c ? c->id : 0;
}
adsp_state_t adsp_conn_state(const adsp_conn_t *c) {
    return c ? c->state : ADSP_STATE_CLOSED;
}
bool adsp_conn_is_open(const adsp_conn_t *c) {
    return c && c->in_use && c->state == ADSP_STATE_OPEN;
}
const atalk_socket_addr_t *adsp_conn_remote(const adsp_conn_t *c) {
    return c ? &c->remote : NULL;
}
uint8_t adsp_conn_local_socket(const adsp_conn_t *c) {
    return c ? c->local_socket : 0;
}
uint16_t adsp_conn_local_cid(const adsp_conn_t *c) {
    return c ? c->local_cid : 0;
}
uint16_t adsp_conn_remote_cid(const adsp_conn_t *c) {
    return c ? c->remote_cid : 0;
}
bool adsp_conn_initiator(const adsp_conn_t *c) {
    return c && c->initiator;
}
uint64_t adsp_conn_bytes_in(const adsp_conn_t *c) {
    return c ? c->bytes_in : 0;
}
uint64_t adsp_conn_bytes_out(const adsp_conn_t *c) {
    return c ? c->bytes_out : 0;
}
uint32_t adsp_conn_send_seq(const adsp_conn_t *c) {
    return c ? c->send_seq : 0;
}
uint32_t adsp_conn_recv_seq(const adsp_conn_t *c) {
    return c ? c->recv_seq : 0;
}
uint32_t adsp_conn_send_wdw_seq(const adsp_conn_t *c) {
    return c ? c->send_wdw_seq : 0;
}
uint16_t adsp_conn_recv_wdw(const adsp_conn_t *c) {
    return c ? c->recv_wdw : 0;
}
int adsp_conn_unacked(const adsp_conn_t *c) {
    return c ? (int)(c->send_seq - c->first_rtmt_seq) : 0;
}
uint64_t adsp_conn_retransmits(const adsp_conn_t *c) {
    return c ? c->retransmits : 0;
}

int adsp_conn_slot_max(const adsp_stack_t *s) {
    (void)s;
    return ADSP_MAX_CONNECTIONS;
}

adsp_conn_t *adsp_conn_at(adsp_stack_t *s, int slot) {
    if (!s || slot < 0 || slot >= ADSP_MAX_CONNECTIONS)
        return NULL;
    return s->conns[slot].in_use ? &s->conns[slot] : NULL;
}

const adsp_stats_t *adsp_get_stats(const adsp_stack_t *s) {
    static const adsp_stats_t empty;
    return s ? &s->stats : &empty;
}

// ============================================================================
// Production instance — DDP transport and scheduler timers
// ============================================================================

static adsp_stack_t *g_adsp;
static scheduler_t *g_adsp_scheduler;
static int g_adsp_event_token;
static uint64_t g_adsp_armed_at = ADSP_NO_DEADLINE;

static uint64_t adsp_host_now(void *ctx) {
    (void)ctx;
    if (!g_adsp_scheduler)
        return 0;
    return (uint64_t)scheduler_time_ns(g_adsp_scheduler);
}

static int adsp_host_send(void *ctx, const atalk_socket_addr_t *dest, uint8_t src_socket, const uint8_t *pkt, int len) {
    (void)ctx;
    return atalk_ddp_send_to(dest, src_socket, DDP_TYPE_ADSP, pkt, len);
}

static void adsp_host_timer_cb(void *source, uint64_t data) {
    (void)source;
    (void)data;
    g_adsp_armed_at = ADSP_NO_DEADLINE;
    adsp_run_timers(g_adsp);
}

// Keep exactly one scheduler event outstanding, at the engine's earliest
// deadline.  Emulated time only — the wire trace is a function of the
// instruction stream, never of the host clock.
static void adsp_host_rearm(void *ctx, uint64_t deadline_ns) {
    (void)ctx;
    if (!g_adsp_scheduler)
        return;
    if (deadline_ns == g_adsp_armed_at)
        return;
    remove_event(g_adsp_scheduler, &adsp_host_timer_cb, NULL);
    g_adsp_armed_at = deadline_ns;
    if (deadline_ns == ADSP_NO_DEADLINE)
        return;
    uint64_t now = adsp_host_now(NULL);
    uint64_t delay = (deadline_ns > now) ? (deadline_ns - now) : 0;
    scheduler_new_cpu_event(g_adsp_scheduler, &adsp_host_timer_cb, &g_adsp_event_token, 0, 0, delay);
}

void atalk_adsp_init(scheduler_t *scheduler) {
    atalk_adsp_shutdown();
    g_adsp_scheduler = scheduler;
    g_adsp_armed_at = ADSP_NO_DEADLINE;
    if (scheduler) {
        // Idempotent: re-registering after a machine rebuild updates in place.
        scheduler_new_event_type(scheduler, "adsp", &g_adsp_event_token, "timer", &adsp_host_timer_cb);
    }
    adsp_config_t cfg = {
        .ctx = NULL,
        .now_ns = adsp_host_now,
        .send = adsp_host_send,
        .rearm = adsp_host_rearm,
    };
    g_adsp = adsp_stack_new(&cfg);
    LOG(3, "ADSP: endpoint ready (DDP type %d)", DDP_TYPE_ADSP);
}

void atalk_adsp_shutdown(void) {
    if (g_adsp) {
        adsp_close_all(g_adsp, "the emulated machine is going away");
        adsp_stack_free(g_adsp);
        g_adsp = NULL;
    }
    if (g_adsp_scheduler)
        remove_event(g_adsp_scheduler, &adsp_host_timer_cb, NULL);
    g_adsp_scheduler = NULL;
    g_adsp_armed_at = ADSP_NO_DEADLINE;
}

adsp_stack_t *atalk_adsp_stack(void) {
    return g_adsp;
}

void atalk_adsp_ddp_in(const ddp_header_t *ddp, const uint8_t *buf, int len) {
    if (!g_adsp || !ddp)
        return;
    atalk_socket_addr_t from = {.net = ddp->src_net, .node = ddp->llap.src, .socket = ddp->src_socket};
    adsp_input(g_adsp, &from, ddp->dst_socket, buf, len);
}

// ============================================================================
// Object model — `appletalk.adsp`
// ============================================================================

static struct object *g_adsp_object;
static struct object *g_adsp_conns_object;
static struct object *g_adsp_stats_object;

// Per-entry instance data: the connection's slot in the engine's table.
typedef struct {
    int slot;
} adsp_slot_data_t;

static adsp_slot_data_t g_adsp_conn_data[ADSP_MAX_CONNECTIONS];
static struct object *g_adsp_conn_objs[ADSP_MAX_CONNECTIONS];

extern const class_desc_t adsp_class;
extern const class_desc_t adsp_conns_class;
extern const class_desc_t adsp_conn_class;
extern const class_desc_t adsp_stats_class;

static adsp_conn_t *adsp_obj_conn(struct object *self) {
    const adsp_slot_data_t *d = (const adsp_slot_data_t *)object_data(self);
    return d ? adsp_conn_at(g_adsp, d->slot) : NULL;
}

// --- appletalk.adsp.connections[i] ------------------------------------------

static value_t adsp_conn_attr_id(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, (uint64_t)adsp_conn_id(adsp_obj_conn(self)));
}
static value_t adsp_conn_attr_state(struct object *self, const member_t *m) {
    (void)m;
    int st = (int)adsp_conn_state(adsp_obj_conn(self));
    if (st < 0 || st >= ADSP_STATE_COUNT)
        st = 0;
    return val_enum(st, ADSP_STATE_NAMES, ADSP_STATE_COUNT);
}
static value_t adsp_conn_attr_role(struct object *self, const member_t *m) {
    (void)m;
    return val_str(adsp_conn_initiator(adsp_obj_conn(self)) ? "initiator" : "responder");
}
static value_t adsp_conn_attr_local_socket(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(1, adsp_conn_local_socket(adsp_obj_conn(self)));
}
static value_t adsp_conn_attr_remote_node(struct object *self, const member_t *m) {
    (void)m;
    const atalk_socket_addr_t *a = adsp_conn_remote(adsp_obj_conn(self));
    return val_uint(1, a ? a->node : 0);
}
static value_t adsp_conn_attr_remote_socket(struct object *self, const member_t *m) {
    (void)m;
    const atalk_socket_addr_t *a = adsp_conn_remote(adsp_obj_conn(self));
    return val_uint(1, a ? a->socket : 0);
}
static value_t adsp_conn_attr_local_cid(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(2, adsp_conn_local_cid(adsp_obj_conn(self)));
}
static value_t adsp_conn_attr_remote_cid(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(2, adsp_conn_remote_cid(adsp_obj_conn(self)));
}
static value_t adsp_conn_attr_send_seq(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, adsp_conn_send_seq(adsp_obj_conn(self)));
}
static value_t adsp_conn_attr_recv_seq(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, adsp_conn_recv_seq(adsp_obj_conn(self)));
}
static value_t adsp_conn_attr_send_wdw_seq(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, adsp_conn_send_wdw_seq(adsp_obj_conn(self)));
}
static value_t adsp_conn_attr_recv_wdw(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(2, adsp_conn_recv_wdw(adsp_obj_conn(self)));
}
static value_t adsp_conn_attr_unacked(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, (uint64_t)adsp_conn_unacked(adsp_obj_conn(self)));
}
static value_t adsp_conn_attr_bytes_in(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(8, adsp_conn_bytes_in(adsp_obj_conn(self)));
}
static value_t adsp_conn_attr_bytes_out(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(8, adsp_conn_bytes_out(adsp_obj_conn(self)));
}
static value_t adsp_conn_attr_retransmits(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(8, adsp_conn_retransmits(adsp_obj_conn(self)));
}

static value_t adsp_conn_method_close(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    adsp_conn_t *c = adsp_obj_conn(self);
    if (!c)
        return val_err("that connection is already closed");
    adsp_close(g_adsp, c, "closed from the object model");
    return val_none();
}

#define ADSP_CONN_ATTR(nm, w, getter, doc_text)                                                                        \
    {                                                                                                                  \
        .kind = M_ATTR, .name = nm, .doc = doc_text, .flags = VAL_RO, .attr = {                                        \
            .type = V_UINT,                                                                                            \
            .width = w,                                                                                                \
            .get = getter                                                                                              \
        }                                                                                                              \
    }

static const member_t adsp_conn_members[] = {
    ADSP_CONN_ATTR("id", 4, adsp_conn_attr_id, "Stable identity of this connection end"),
    {.kind = M_ATTR,
                                                                            .name = "state",
                                                                            .doc = "Connection-end state (Inside AppleTalk 12-5)",
                                                                            .flags = VAL_RO,
                                                                            .attr = {.type = V_ENUM, .enum_values = ADSP_STATE_NAMES, .get = adsp_conn_attr_state}},
    {.kind = M_ATTR,
                                                                            .name = "role",
                                                                            .doc = "initiator if we sent the first open request, else responder",
                                                                            .flags = VAL_RO,
                                                                            .attr = {.type = V_STRING, .get = adsp_conn_attr_role}},
    ADSP_CONN_ATTR("local_socket", 1, adsp_conn_attr_local_socket, "Socket this end owns"),
    ADSP_CONN_ATTR("remote_node", 1, adsp_conn_attr_remote_node, "LLAP node of the remote end"),
    ADSP_CONN_ATTR("remote_socket", 1, adsp_conn_attr_remote_socket, "Socket of the remote end"),
    ADSP_CONN_ATTR("local_cid", 2, adsp_conn_attr_local_cid, "Our ConnID"),
    ADSP_CONN_ATTR("remote_cid", 2, adsp_conn_attr_remote_cid, "The remote end's ConnID"),
    ADSP_CONN_ATTR("send_seq", 4, adsp_conn_attr_send_seq, "SendSeq — next new byte we will send"),
    ADSP_CONN_ATTR("recv_seq", 4, adsp_conn_attr_recv_seq, "RecvSeq — next byte we expect"),
    ADSP_CONN_ATTR("send_wdw_seq", 4, adsp_conn_attr_send_wdw_seq, "SendWdwSeq — last byte the peer has room for"),
    ADSP_CONN_ATTR("recv_wdw", 2, adsp_conn_attr_recv_wdw, "RecvWdw — bytes we advertise room for"),
    ADSP_CONN_ATTR("unacked", 4, adsp_conn_attr_unacked, "Bytes sent but not yet acknowledged"),
    ADSP_CONN_ATTR("bytes_in", 8, adsp_conn_attr_bytes_in, "Stream bytes accepted on this connection"),
    ADSP_CONN_ATTR("bytes_out", 8, adsp_conn_attr_bytes_out, "Stream bytes transmitted on this connection"),
    ADSP_CONN_ATTR("retransmits", 8, adsp_conn_attr_retransmits, "Retransmission events on this connection"),
    {.kind = M_METHOD,
                                                                            .name = "close",
                                                                            .doc = "Send close advice and drop this connection end",
                                                                            .method = {.args = NULL,
                .nargs = 0,
                .result = V_NONE,
                .fn = adsp_conn_method_close,
                .ui_flags = MM_DESTRUCTIVE | MM_MUTATE}},
};

const class_desc_t adsp_conn_class = {
    .name = "adsp_connection",
    .members = adsp_conn_members,
    .n_members = ARRAY_LEN(adsp_conn_members),
};

// --- appletalk.adsp.connections ---------------------------------------------

static struct object *adsp_conns_get(struct object *self, int index) {
    (void)self;
    if (index < 0 || index >= ADSP_MAX_CONNECTIONS || !adsp_conn_at(g_adsp, index))
        return NULL;
    return g_adsp_conn_objs[index];
}
static int adsp_conns_count(struct object *self) {
    (void)self;
    int n = 0;
    for (int i = 0; i < ADSP_MAX_CONNECTIONS; i++)
        if (adsp_conn_at(g_adsp, i))
            n++;
    return n;
}
static int adsp_conns_next(struct object *self, int prev) {
    (void)self;
    for (int i = prev + 1; i < ADSP_MAX_CONNECTIONS; i++)
        if (adsp_conn_at(g_adsp, i))
            return i;
    return -1;
}

// Collections publish their live size as an attribute: scripts assert on it
// directly instead of probing indices with try().
static value_t adsp_conns_attr_count(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, (uint64_t)adsp_conns_count(self));
}

static const member_t adsp_conns_members[] = {
    {.kind = M_ATTR,
     .name = "count",
     .doc = "Live ADSP connection ends",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .width = 4, .get = adsp_conns_attr_count}},
    {.kind = M_CHILD,
     .name = "entries",
     .doc = "Live ADSP connection ends",
     .child = {.cls = &adsp_conn_class,
               .indexed = true,
               .get = adsp_conns_get,
               .count = adsp_conns_count,
               .next = adsp_conns_next}},
};

const class_desc_t adsp_conns_class = {
    .name = "adsp_connections",
    .members = adsp_conns_members,
    .n_members = ARRAY_LEN(adsp_conns_members),
};

// --- appletalk.adsp.stats ----------------------------------------------------

static value_t adsp_stats_attr(struct object *self, const member_t *m) {
    (void)self;
    const adsp_stats_t *st = adsp_get_stats(g_adsp);
    size_t offset = (size_t)(uintptr_t)m->attr.user_data;
    return val_uint(8, *(const uint64_t *)((const uint8_t *)st + offset));
}

#define ADSP_STAT_MEMBER(field, doc_text)                                                                              \
    {                                                                                                                  \
        .kind = M_ATTR, .name = #field, .doc = doc_text, .flags = VAL_RO, .attr = {                                    \
            .type = V_UINT,                                                                                            \
            .width = 8,                                                                                                \
            .get = adsp_stats_attr,                                                                                    \
            .user_data = (const void *)(uintptr_t)offsetof(adsp_stats_t, field)                                        \
        }                                                                                                              \
    }

static const member_t adsp_stats_members[] = {
    ADSP_STAT_MEMBER(packets_in, "ADSP packets accepted from the wire"),
    ADSP_STAT_MEMBER(packets_out, "ADSP packets put on the wire"),
    ADSP_STAT_MEMBER(bytes_in, "Stream bytes delivered to clients"),
    ADSP_STAT_MEMBER(bytes_out, "Stream bytes transmitted"),
    ADSP_STAT_MEMBER(opens, "Connections that reached the open state"),
    ADSP_STAT_MEMBER(open_denials, "Open requests denied, in either direction"),
    ADSP_STAT_MEMBER(retransmits, "Retransmission events"),
    ADSP_STAT_MEMBER(out_of_sequence, "Data packets discarded as out of sequence"),
    ADSP_STAT_MEMBER(forward_resets, "Forward resets sent or accepted"),
    ADSP_STAT_MEMBER(attentions_in, "Attention messages accepted"),
    ADSP_STAT_MEMBER(attentions_out, "Attention messages sent"),
    ADSP_STAT_MEMBER(timeouts, "Connection ends torn down by the connection timer"),
};

const class_desc_t adsp_stats_class = {
    .name = "adsp_stats",
    .members = adsp_stats_members,
    .n_members = ARRAY_LEN(adsp_stats_members),
};

// --- appletalk.adsp ----------------------------------------------------------

// `max_data` is a constant of the protocol; a getter keeps it in the tree
// without a backing variable.
static value_t adsp_attr_max_data(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(2, ADSP_MAX_DATA);
}

static const member_t adsp_members[] = {
    {.kind = M_ATTR,
     .name = "max_data",
     .doc = "ADSP data bytes per packet (Inside AppleTalk 12-12)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .width = 2, .get = adsp_attr_max_data}},
};

const class_desc_t adsp_class = {
    .name = "adsp",
    .members = adsp_members,
    .n_members = ARRAY_LEN(adsp_members),
};

void atalk_adsp_install_objects(struct object *parent) {
    if (!parent || g_adsp_object)
        return;
    g_adsp_object = object_new(&adsp_class, NULL, "adsp");
    if (!g_adsp_object)
        return;
    object_set_category(g_adsp_object, M_CAT_ADVANCED);
    object_attach(parent, g_adsp_object);

    g_adsp_conns_object = object_new(&adsp_conns_class, NULL, "connections");
    if (g_adsp_conns_object)
        object_attach(g_adsp_object, g_adsp_conns_object);
    g_adsp_stats_object = object_new(&adsp_stats_class, NULL, "stats");
    if (g_adsp_stats_object)
        object_attach(g_adsp_object, g_adsp_stats_object);

    // Entry objects are handed out by the collection callbacks and never
    // attached, so the cascade delete does not free them (we do, below).
    for (int i = 0; i < ADSP_MAX_CONNECTIONS; i++) {
        g_adsp_conn_data[i].slot = i;
        g_adsp_conn_objs[i] = object_new(&adsp_conn_class, &g_adsp_conn_data[i], NULL);
    }
}

void atalk_adsp_remove_objects(void) {
    for (int i = 0; i < ADSP_MAX_CONNECTIONS; i++) {
        if (g_adsp_conn_objs[i])
            object_delete(g_adsp_conn_objs[i]);
        g_adsp_conn_objs[i] = NULL;
    }
    struct object **nodes[] = {&g_adsp_conns_object, &g_adsp_stats_object, &g_adsp_object};
    for (int i = 0; i < ARRAY_LEN(nodes); i++) {
        if (!*nodes[i])
            continue;
        object_detach(*nodes[i]);
        object_delete(*nodes[i]);
        *nodes[i] = NULL;
    }
}
