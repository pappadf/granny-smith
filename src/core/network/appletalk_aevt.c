// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_aevt.c
// The `appletalk.aevt` surface: send Apple events to guest applications, take
// delivery of the ones they send us, and publish both as object-model state.
//
// Coding reference: docs/internals/core/network/ppc_appleevents.md §5 (high-level event
// framing), §6 (the map and text forms), §7 (what we implement) and §8 (this
// surface).  The codec itself lives in appletalk_aevt_codec.c; the session
// layer under it is appletalk_ppc.c.
//
// The central design decision (§8): `send` never blocks.  A reply can only
// arrive while the guest runs, and the script owns the scheduler, so a send
// returns an event object whose `state` the script polls between budget runs.
// Timeouts are therefore instruction budgets, evaluated lazily against the
// retired-instruction count — no wall clock anywhere.

// ============================================================================
// Includes
// ============================================================================

#include "appletalk_aevt.h"

#include "appletalk.h"
#include "appletalk_ppc.h"
#include "common.h"
#include "log.h"
#include "object.h"
#include "scheduler.h"
#include "value.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("aevt");

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

// ============================================================================
// Constants and Macros
// ============================================================================

#define AEVT_MAX_EVENTS 256 // events remembered per run (append-only, §8)
#define AEVT_MAX_INBOX  32

#define AEVT_HLE_HEADER_SIZE 36 // HighLevelEventMsg (§5.1)
#define AEVT_HLE_VERSION     3
#define AEVT_HLE_WHAT        23 // kHighLevelEvent
#define AEVT_MODIFIER_REPLY  0x0001 // "this message is a reply" (§5.1)

// Default reply budget: generous enough for an application to be launched by
// the event it is being sent.
#define AEVT_DEFAULT_TIMEOUT_INSTR 20000000ull

typedef enum {
    AEVT_STATE_QUEUED = 0, // waiting for a port or a session
    AEVT_STATE_SENT,
    AEVT_STATE_REPLIED,
    AEVT_STATE_ERROR,
    AEVT_STATE_TIMEOUT,
} aevt_state_t;

static const char *const AEVT_STATE_NAMES[] = {"queued", "sent", "replied", "error", "timeout", NULL};
#define AEVT_STATE_COUNT 5

// ============================================================================
// Type Definitions
// ============================================================================

// One outgoing event and everything a script can ask about it.
typedef struct {
    bool in_use;
    int slot;
    uint32_t return_id; // correlates the reply (§5.1)
    aevt_state_t state;
    char target[33];
    char tag[33];
    char class4[5];
    char id4[5];
    char *text; // the request, text form
    char *error; // why it failed, when it did
    value_t request; // the request map (owned)
    value_t reply; // the reply map (owned; V_NONE until one arrives)
    int64_t errn;
    bool no_reply; // fire and forget
    uint64_t sent_at_instr;
    uint64_t timeout_instr;
    ppc_session_t *session;
} aevt_event_t;

// One event a guest sent us.
typedef struct {
    bool in_use;
    int slot;
    char sender[33];
    char class4[5];
    char id4[5];
    uint32_t return_id;
    value_t map; // owned
    char *text; // owned
} aevt_inbox_t;

// ============================================================================
// Module state
// ============================================================================

// The Apple-event layer's part of the network: the reply the host port
// sends on its own (the port itself is PPC's), and the `appletalk.aevt`
// collections' entries.  atalk_aevt_init makes it; the network owns it.
struct aevt_host {
    char auto_reply[256];
    object_cache_t event_entries;
    object_cache_t inbox_entries;
};

// The network's, set by atalk_aevt_init.
static aevt_host_t *g_host;

// Counters published as `appletalk.aevt.stats`.
typedef struct {
    uint64_t sent;
    uint64_t replied;
    uint64_t errors;
    uint64_t timeouts;
    uint64_t received;
    uint64_t auto_replies;
    uint64_t malformed; // blocks that are no high-level event
    uint64_t dropped; // events received with the inbox full (answered, not kept)
} aevt_stats_t;

// The Apple-event layer's part of a machine's connection (atalk_conn_t): the
// events a script sent that Mac, the ones it sent us, and their counters.
struct aevt_link {
    aevt_event_t events[AEVT_MAX_EVENTS];
    int event_count; // append-only high-water mark
    aevt_inbox_t inbox[AEVT_MAX_INBOX];
    int inbox_count;
    uint32_t next_return_id;
    aevt_stats_t stats;
};

// The link of the connection plugged into the network.  While none is, this
// points at an empty link no event is added to (sends are refused).
static aevt_link_t g_no_link;
static aevt_link_t *g_aevt = &g_no_link;

// ============================================================================
// Forward declarations
// ============================================================================

static void aevt_flush_session(ppc_session_t *s);
static void aevt_settle(aevt_event_t *ev);
static bool aevt_dispatch(aevt_event_t *ev, char *err, size_t err_len);

// ============================================================================
// Operations — small helpers
// ============================================================================

// ============================================================================
// Operations — the event table
// ============================================================================

static aevt_event_t *aevt_alloc_event(void) {
    // Events are append-only for the life of the connection so the V_OBJECT a
    // send returns stays valid in a `let` binding (§8).
    if (g_aevt == &g_no_link || g_aevt->event_count >= AEVT_MAX_EVENTS)
        return NULL;
    aevt_event_t *ev = &g_aevt->events[g_aevt->event_count];
    memset(ev, 0, sizeof(*ev));
    ev->in_use = true;
    ev->slot = g_aevt->event_count++;
    ev->return_id = g_aevt->next_return_id++;
    ev->reply = val_none();
    ev->request = val_none();
    return ev;
}

static void aevt_event_free_contents(aevt_event_t *ev) {
    free(ev->text);
    free(ev->error);
    value_free(&ev->request);
    value_free(&ev->reply);
    memset(ev, 0, sizeof(*ev));
}

static void aevt_fail(aevt_event_t *ev, const char *reason) {
    if (!ev || ev->state == AEVT_STATE_REPLIED)
        return;
    ev->state = AEVT_STATE_ERROR;
    free(ev->error);
    ev->error = strdup(reason ? reason : "the event failed");
    aevt_settle(ev);
    g_aevt->stats.errors++;
    LOG(3, "AE: event %u to '%s' failed — %s", (unsigned)ev->return_id, ev->target, reason ? reason : "");
}

// A pending event expires against retired instructions, not wall time (§7),
// and it is evaluated lazily: reading the state is what notices.
static aevt_state_t aevt_effective_state(aevt_event_t *ev) {
    if (!ev)
        return AEVT_STATE_ERROR;
    if (ev->state == AEVT_STATE_SENT && ev->timeout_instr > 0) {
        uint64_t now = cpu_instr_count();
        if (now > ev->sent_at_instr && now - ev->sent_at_instr > ev->timeout_instr) {
            ev->state = AEVT_STATE_TIMEOUT;
            free(ev->error);
            ev->error = strdup("no reply within the instruction budget");
            aevt_settle(ev);
            g_aevt->stats.timeouts++;
        }
    }
    return ev->state;
}

static aevt_event_t *aevt_find_by_return_id(uint32_t return_id) {
    for (int i = 0; i < g_aevt->event_count; i++) {
        if (g_aevt->events[i].in_use && g_aevt->events[i].return_id == return_id)
            return &g_aevt->events[i];
    }
    return NULL;
}

// ============================================================================
// Operations — building and sending
// ============================================================================

// Frame an encoded event as a high-level event message block (§5.1, §4.7) and
// hand it to the session layer.
static bool aevt_write_event(ppc_session_t *s, const char *class4, const char *id4, uint32_t return_id, bool is_reply,
                             const uint8_t *stream, int stream_len, char *err, size_t err_len) {
    uint8_t msg[AEVT_HLE_HEADER_SIZE + AEVT_MAX_STREAM];
    if (stream_len < 0 || stream_len > AEVT_MAX_STREAM) {
        snprintf(err, err_len, "the event is too large to send");
        return false;
    }
    memset(msg, 0, AEVT_HLE_HEADER_SIZE);
    WR_BE16(&msg[0], AEVT_HLE_HEADER_SIZE);
    WR_BE16(&msg[2], AEVT_HLE_VERSION);
    WR_BE32(&msg[4], 0); // reserved
    // The embedded EventRecord is a header, not an event (§5.1).
    WR_BE16(&msg[8], AEVT_HLE_WHAT);
    WR_BE32(&msg[10], fourcc_value(class4)); // message = event class
    WR_BE32(&msg[14], 0); // when: the receiver stamps it
    WR_BE32(&msg[18], fourcc_value(id4)); // where = event ID
    WR_BE16(&msg[22], is_reply ? AEVT_MODIFIER_REPLY : 0);
    WR_BE32(&msg[24], return_id);
    WR_BE32(&msg[28], 0); // posting options
    WR_BE32(&msg[32], (uint32_t)stream_len);
    if (stream_len > 0)
        memcpy(&msg[AEVT_HLE_HEADER_SIZE], stream, (size_t)stream_len);

    if (atalk_ppc_send_block(s, fourcc_value(class4), fourcc_value(id4), return_id, msg,
                             AEVT_HLE_HEADER_SIZE + stream_len) != 0) {
        snprintf(err, err_len, "the session would not take the message");
        return false;
    }
    return true;
}

// Encode and write a pending event on its (now open) session.
static bool aevt_dispatch(aevt_event_t *ev, char *err, size_t err_len) {
    uint8_t stream[AEVT_MAX_STREAM];
    int len = aevt_encode(&ev->request, stream, (int)sizeof(stream), err, err_len);
    if (len < 0)
        return false;
    if (!aevt_write_event(ev->session, ev->class4, ev->id4, ev->return_id, false, stream, len, err, err_len))
        return false;
    ev->state = ev->no_reply ? AEVT_STATE_REPLIED : AEVT_STATE_SENT;
    ev->sent_at_instr = cpu_instr_count();
    g_aevt->stats.sent++;
    LOG(3, "AE: sent %s/%s to '%s' (return id %u)", ev->class4, ev->id4, ev->target, (unsigned)ev->return_id);
    return true;
}

// ============================================================================
// Operations — session callbacks
// ============================================================================

static void aevt_session_ready(void *ctx, ppc_session_t *s) {
    (void)ctx;
    aevt_flush_session(s);
}

static void aevt_session_failed(void *ctx, ppc_session_t *s, const char *reason) {
    (void)ctx;
    for (int i = 0; i < g_aevt->event_count; i++) {
        aevt_event_t *ev = &g_aevt->events[i];
        if (ev->in_use && ev->session == s && (ev->state == AEVT_STATE_QUEUED || ev->state == AEVT_STATE_SENT))
            aevt_fail(ev, reason);
    }
}

static void aevt_session_closed(void *ctx, ppc_session_t *s) {
    (void)ctx;
    for (int i = 0; i < g_aevt->event_count; i++) {
        aevt_event_t *ev = &g_aevt->events[i];
        if (ev->in_use && ev->session == s) {
            if (ev->state == AEVT_STATE_SENT || ev->state == AEVT_STATE_QUEUED)
                aevt_fail(ev, "the session closed before a reply arrived");
            ev->session = NULL;
        }
    }
}

// A message block on any session: unwrap the high-level event header and hand
// the stream on (§5.1).
static void aevt_session_block(void *ctx, ppc_session_t *s, uint32_t creator, uint32_t type, uint32_t user_data,
                               const uint8_t *payload, int len) {
    (void)ctx;
    (void)user_data;
    if (len < AEVT_HLE_HEADER_SIZE) {
        g_aevt->stats.malformed++;
        LOG(3, "AE: a %d-byte block is too short to be a high-level event", len);
        return;
    }
    uint16_t header_len = RD_BE16(&payload[0]);
    uint16_t version = RD_BE16(&payload[2]);
    if (header_len < AEVT_HLE_HEADER_SIZE || header_len > len) {
        g_aevt->stats.malformed++;
        LOG(3, "AE: high-level event header length %u is not usable", (unsigned)header_len);
        return;
    }
    if (version > AEVT_HLE_VERSION)
        LOG(3, "AE: high-level event version %u is newer than we know", (unsigned)version);

    uint16_t modifiers = RD_BE16(&payload[22]);
    uint32_t return_id = RD_BE32(&payload[24]);
    uint32_t msg_len = RD_BE32(&payload[32]);
    // The class and ID live in the EventRecord overlay; the block header
    // carries them too, and we trust the overlay (§5.1).
    char class4[5], id4[5];
    fourcc_text(RD_BE32(&payload[10]), class4);
    fourcc_text(RD_BE32(&payload[18]), id4);
    if (class4[0] == '\0')
        fourcc_text(creator, class4);
    if (id4[0] == '\0')
        fourcc_text(type, id4);

    const uint8_t *stream = payload + header_len;
    int avail = len - header_len;
    if (msg_len > (uint32_t)avail) {
        LOG(3, "AE: the event claims %u bytes but the block holds %d", (unsigned)msg_len, avail);
        return;
    }

    atalk_aevt_deliver(s, atalk_ppc_session_port(s), class4, id4, return_id, (modifiers & AEVT_MODIFIER_REPLY) != 0,
                       stream, (int)msg_len);
}

static const ppc_client_t g_session_client = {
    .on_ready = aevt_session_ready,
    .on_failed = aevt_session_failed,
    .on_block = aevt_session_block,
    .on_closed = aevt_session_closed,
};

// ============================================================================
// Operations — delivery
// ============================================================================

// Answer an inbox event with the configured template, if there is one (§8).
static void aevt_send_auto_reply(ppc_session_t *s, uint32_t return_id) {
    if (!s)
        return;
    char err[192] = "";
    value_t reply;
    if (g_host->auto_reply[0]) {
        reply = aevt_parse_text(g_host->auto_reply, err, sizeof(err));
        if (val_is_error(&reply)) {
            LOG(2, "AE: the auto-reply template does not parse — %s", err);
            value_free(&reply);
            return;
        }
    } else {
        // The generic answer: a reply event carrying no error (§5.5).
        reply = aevt_parse_text("aevt/ansr{errn:0}", err, sizeof(err));
        if (val_is_error(&reply)) {
            value_free(&reply);
            return;
        }
    }
    char class4[5], id4[5];
    if (!aevt_event_codes(&reply, class4, id4)) {
        value_free(&reply);
        return;
    }
    uint8_t stream[AEVT_MAX_STREAM];
    int len = aevt_encode(&reply, stream, (int)sizeof(stream), err, sizeof(err));
    if (len < 0) {
        LOG(2, "AE: the auto-reply will not encode — %s", err);
        value_free(&reply);
        return;
    }
    if (aevt_write_event(s, class4, id4, return_id, true, stream, len, err, sizeof(err)))
        g_aevt->stats.auto_replies++;
    else
        LOG(2, "AE: the auto-reply could not be sent — %s", err);
    value_free(&reply);
}

void atalk_aevt_deliver(ppc_session_t *session, const char *sender, const char *class4, const char *id4,
                        uint32_t return_id, bool is_reply, const uint8_t *stream, int len) {
    value_t map = aevt_decode(class4, id4, stream, len);

    // What makes an arriving event a reply is that its return ID matches one
    // we are waiting on (§5.5) — that is the correlation the Apple Event
    // Manager itself uses.  System 7.5's Finder answers with the reply bit in
    // `modifiers` clear, so treating that bit as authoritative sends genuine
    // replies to the inbox and leaves the caller waiting for ever.  The bit
    // and the reply class are corroboration, not the test.
    aevt_event_t *pending = aevt_find_by_return_id(return_id);
    if (pending && pending->state != AEVT_STATE_SENT)
        pending = NULL; // already settled; a late duplicate is not its reply
    if (pending || is_reply) {
        aevt_event_t *ev = pending;
        if (!ev) {
            LOG(3, "AE: a reply arrived for unknown return id %u", (unsigned)return_id);
            value_free(&map);
            return;
        }
        if (val_is_error(&map)) {
            const char *why = val_as_str(&map);
            aevt_fail(ev, why ? why : "the reply could not be decoded");
            value_free(&map);
            return;
        }
        value_free(&ev->reply);
        ev->reply = map; // ownership moves to the event
        ev->errn = aevt_reply_errn(&ev->reply);
        ev->state = AEVT_STATE_REPLIED;
        aevt_settle(ev);
        g_aevt->stats.replied++;
        LOG(3, "AE: event %u replied (errn=%lld)", (unsigned)return_id, (long long)ev->errn);
        return;
    }

    // Not a reply: this is an event the guest sent us.  It is answered on the
    // session it came in on, so the sender's AESend completes -- even when the
    // inbox is full and it is not kept.  A full inbox returned without
    // answering: the guest waited out its own timeout, and nothing counted the
    // loss.
    g_aevt->stats.received++;
    if (g_aevt->inbox_count >= AEVT_MAX_INBOX) {
        g_aevt->stats.dropped++;
        LOG(2, "AE: the inbox is full; answering but not keeping %s/%s (inbox.clear() makes room)", class4, id4);
        value_free(&map);
        aevt_send_auto_reply(session, return_id);
        return;
    }
    aevt_inbox_t *in = &g_aevt->inbox[g_aevt->inbox_count];
    memset(in, 0, sizeof(*in));
    in->in_use = true;
    in->slot = g_aevt->inbox_count++;
    snprintf(in->sender, sizeof(in->sender), "%s", sender ? sender : "");
    snprintf(in->class4, sizeof(in->class4), "%s", class4 ? class4 : "");
    snprintf(in->id4, sizeof(in->id4), "%s", id4 ? id4 : "");
    in->return_id = return_id;
    in->map = map;
    in->text = val_is_error(&map) ? NULL : aevt_render_text(&map);
    LOG(3, "AE: received %s/%s from '%s'", in->class4, in->id4, in->sender);
    // The session itself, not its id looked up again: the id went through a
    // 16-bit parameter and back against 32 bits.
    aevt_send_auto_reply(session, return_id);
}

// ============================================================================
// Operations — the send path
// ============================================================================

// Every event gets its own session.
//
// Reusing an open session looks like the obvious optimisation and does not
// work: a System 7 application services the session its PPCInform accepted
// for that transaction, and once it has answered it stops reading, without
// closing anything.  Our ADSP connection stays up, so a second event written
// to that session is accepted by the transport and then silently ignored —
// the send simply never gets a reply.  Observed against both the 7.1 and the
// 7.5 Finder: the first event on a session is answered, every later one is
// not, and a fresh session is answered again.
static ppc_session_t *aevt_session_for(const char *port, char *err, size_t err_len) {
    return atalk_ppc_open(port, &g_session_client, NULL, err, err_len);
}

// An event has reached a final state, so its session has done its job.
static void aevt_settle(aevt_event_t *ev) {
    if (!ev || !ev->session)
        return;
    ppc_session_t *s = ev->session;
    ev->session = NULL;
    // Any other event still riding this session loses it too.
    for (int i = 0; i < g_aevt->event_count; i++)
        if (g_aevt->events[i].in_use && g_aevt->events[i].session == s)
            g_aevt->events[i].session = NULL;
    atalk_ppc_close(s, "the event it carried has been answered");
}

// A session finished opening: send whatever was waiting on *that* session.
//
// It matters that this is keyed on the session and not on the target port.
// Every event opens a session of its own (§7.2), so flushing "everything
// queued for this port" onto whichever session happened to become ready
// stranded the other events' sessions — nothing ever closed them — and piled
// several events into one session, which the application services only once.
// The stress test caught it as a leaked initiator session.
static void aevt_flush_session(ppc_session_t *s) {
    for (int i = 0; i < g_aevt->event_count; i++) {
        aevt_event_t *ev = &g_aevt->events[i];
        if (!ev->in_use || ev->state != AEVT_STATE_QUEUED || ev->session != s)
            continue;
        char err[192] = "";
        if (!aevt_dispatch(ev, err, sizeof(err)))
            aevt_fail(ev, err);
    }
}

// Start an event on its way, queueing it if the session is still opening.
static void aevt_begin(aevt_event_t *ev) {
    char err[192] = "";
    ppc_session_t *s = aevt_session_for(ev->target, err, sizeof(err));
    if (!s) {
        aevt_fail(ev, err);
        return;
    }
    ev->session = s;
    if (atalk_ppc_session_state(s) != PPC_SESSION_OPEN) {
        // The dialog is still in flight; aevt_session_ready picks this up.
        ev->state = AEVT_STATE_QUEUED;
        return;
    }
    if (!aevt_dispatch(ev, err, sizeof(err)))
        aevt_fail(ev, err);
}

// ============================================================================
// Lifecycle
// ============================================================================

// Empty the inbox.  Its entries are never handed to a script as bindings, so
// nothing can be left holding one.
static void aevt_inbox_clear(void) {
    for (int i = 0; i < AEVT_MAX_INBOX; i++) {
        if (!g_aevt->inbox[i].in_use)
            continue;
        value_free(&g_aevt->inbox[i].map);
        free(g_aevt->inbox[i].text);
        memset(&g_aevt->inbox[i], 0, sizeof(g_aevt->inbox[i]));
    }
    g_aevt->inbox_count = 0;
}

// Once, when the network comes up: take the PPC layer's inbound events and
// publish the host port.
aevt_host_t *atalk_aevt_init(void) {
    aevt_host_t *host = calloc(1, sizeof(*host));
    if (!host)
        return NULL;
    g_host = host;
    atalk_ppc_set_inbound_client(&g_session_client, NULL);
    char err[192] = "";
    if (atalk_ppc_set_host_port(NULL, true, err, sizeof(err)) != 0)
        LOG(2, "AE: the host port could not be published — %s", err);
    return host;
}

aevt_link_t *atalk_aevt_link_new(void) {
    aevt_link_t *link = calloc(1, sizeof(*link));
    if (link)
        link->next_return_id = 1;
    return link;
}

void atalk_aevt_link_free(aevt_link_t *link) {
    if (!link)
        return;
    GS_ASSERT(link != g_aevt);
    free(link);
}

void atalk_aevt_plug(aevt_link_t *link) {
    if (!link && g_aevt != &g_no_link) {
        // The Mac is gone, and with it every event to or from it.
        for (int i = 0; i < AEVT_MAX_EVENTS; i++)
            if (g_aevt->events[i].in_use)
                aevt_event_free_contents(&g_aevt->events[i]);
        memset(g_aevt->events, 0, sizeof(g_aevt->events));
        g_aevt->event_count = 0;
        aevt_inbox_clear();
    }
    g_aevt = link ? link : &g_no_link;
}

// ============================================================================
// Object model — `appletalk.aevt`
// ============================================================================

static const class_desc_t aevt_class;
static const class_desc_t aevt_events_class;
static const class_desc_t aevt_event_class;
static const class_desc_t aevt_inbox_class;
static const class_desc_t aevt_inbox_entry_class;
static const class_desc_t aevt_stats_class;

static int aevt_obj_slot(struct object *self) {
    return object_entry_index(self);
}

static aevt_event_t *aevt_obj_event(struct object *self) {
    int slot = aevt_obj_slot(self);
    if (slot < 0 || slot >= g_aevt->event_count || !g_aevt->events[slot].in_use)
        return NULL;
    return &g_aevt->events[slot];
}

// --- appletalk.aevt.events[i] ------------------------------------------------

static DEF_GETTER(aevt_event_attr_state) {
    aevt_event_t *ev = aevt_obj_event(self);
    int st = ev ? (int)aevt_effective_state(ev) : 0;
    return val_enum(st, AEVT_STATE_NAMES, AEVT_STATE_COUNT);
}
static DEF_GETTER(aevt_event_attr_target) {
    aevt_event_t *ev = aevt_obj_event(self);
    return val_str(ev ? ev->target : "");
}
static DEF_GETTER(aevt_event_attr_tag) {
    aevt_event_t *ev = aevt_obj_event(self);
    return val_str(ev ? ev->tag : "");
}
static DEF_GETTER(aevt_event_attr_text) {
    aevt_event_t *ev = aevt_obj_event(self);
    return val_str(ev && ev->text ? ev->text : "");
}
static DEF_GETTER(aevt_event_attr_class) {
    aevt_event_t *ev = aevt_obj_event(self);
    return val_str(ev ? ev->class4 : "");
}
static DEF_GETTER(aevt_event_attr_id) {
    aevt_event_t *ev = aevt_obj_event(self);
    return val_str(ev ? ev->id4 : "");
}
static DEF_GETTER(aevt_event_attr_reply) {
    aevt_event_t *ev = aevt_obj_event(self);
    if (!ev || ev->reply.kind != V_MAP)
        return val_map(NULL, 0);
    return value_dup(&ev->reply);
}
static DEF_GETTER(aevt_event_attr_request) {
    aevt_event_t *ev = aevt_obj_event(self);
    if (!ev || ev->request.kind != V_MAP)
        return val_map(NULL, 0);
    return value_dup(&ev->request);
}
static DEF_GETTER(aevt_event_attr_errn) {
    aevt_event_t *ev = aevt_obj_event(self);
    return val_int(ev ? ev->errn : 0);
}
static DEF_GETTER(aevt_event_attr_error) {
    aevt_event_t *ev = aevt_obj_event(self);
    if (ev)
        aevt_effective_state(ev); // a lazy timeout produces the message
    return val_str(ev && ev->error ? ev->error : "");
}
static DEF_GETTER(aevt_event_attr_return_id) {
    aevt_event_t *ev = aevt_obj_event(self);
    return val_uint(4, ev ? ev->return_id : 0);
}

static const member_t aevt_event_members[] = {
    {.kind = M_ATTR,
     .name = "state",
     .doc = "queued, sent, replied, error or timeout",
     .attr = {.type = V_ENUM, .enum_values = AEVT_STATE_NAMES, .get = aevt_event_attr_state}                                },
    {.kind = M_ATTR,
     .name = "target",
     .doc = "The program-linking port this event was addressed to",
     .attr = {.type = V_STRING, .get = aevt_event_attr_target}                                                              },
    {.kind = M_ATTR,
     .name = "tag",
     .doc = "Lookup key given at send time, if any",
     .attr = {.type = V_STRING, .get = aevt_event_attr_tag}                                                                 },
    {.kind = M_ATTR, .name = "class", .doc = "Event class",         .attr = {.type = V_STRING, .get = aevt_event_attr_class}},
    {.kind = M_ATTR, .name = "id",    .doc = "Event ID",            .attr = {.type = V_STRING, .get = aevt_event_attr_id}   },
    {.kind = M_ATTR,
     .name = "text",
     .doc = "The request in text form",
     .attr = {.type = V_STRING, .get = aevt_event_attr_text}                                                                },
    {.kind = M_ATTR,
     .name = "request",
     .doc = "The request as a map",
     .attr = {.type = V_MAP, .get = aevt_event_attr_request}                                                                },
    {.kind = M_ATTR,
     .name = "reply",
     .doc = "The reply as a map; empty until one arrives",
     .attr = {.type = V_MAP, .get = aevt_event_attr_reply}                                                                  },
    {.kind = M_ATTR,
     .name = "errn",
     .doc = "keyErrorNumber from the reply; 0 means success",
     .attr = {.type = V_INT, .get = aevt_event_attr_errn}                                                                   },
    {.kind = M_ATTR,
     .name = "error",
     .doc = "Why the event failed, when it did",
     .attr = {.type = V_STRING, .get = aevt_event_attr_error}                                                               },
    {.kind = M_ATTR,
     .name = "return_id",
     .doc = "The return ID that correlates the reply",
     .attr = {.type = V_UINT, .width = 4, .get = aevt_event_attr_return_id}                                                 },
};

static const class_desc_t aevt_event_class = {
    .name = "aevt_event",
    .members = aevt_event_members,
    .n_members = ARRAY_LEN(aevt_event_members),
};

// --- appletalk.aevt.events ---------------------------------------------------

static struct object *aevt_events_get(struct object *self, int index) {
    (void)self;
    if (index < 0 || index >= g_aevt->event_count || !g_aevt->events[index].in_use)
        return NULL;
    return object_cache_at(&g_host->event_entries, index, NULL);
}
// Name lookup resolves the `tag:` given at send time (§8).
static struct object *aevt_events_lookup(struct object *self, const char *name) {
    (void)self;
    for (int i = 0; i < g_aevt->event_count; i++)
        if (g_aevt->events[i].in_use && g_aevt->events[i].tag[0] && !strcmp(g_aevt->events[i].tag, name))
            return object_cache_at(&g_host->event_entries, i, NULL);
    return NULL;
}

static const collection_desc_t aevt_events_entries = {
    .entry = &aevt_event_class,
    .by_index = {.get = aevt_events_get, .slots = AEVT_MAX_EVENTS},
    .by_key = {.lookup = aevt_events_lookup}
};

static const member_t aevt_events_members[] = {
    OBJ_ENTRIES(&aevt_events_entries, NULL),
};

static const class_desc_t aevt_events_class = {
    .name = "aevt_events",
    .doc = "Apple events sent from the host, by index or by tag",
    .members = aevt_events_members,
    .n_members = ARRAY_LEN(aevt_events_members),
};

// --- appletalk.aevt.inbox[i] -------------------------------------------------

static aevt_inbox_t *aevt_obj_inbox(struct object *self) {
    int slot = aevt_obj_slot(self);
    if (slot < 0 || slot >= g_aevt->inbox_count || !g_aevt->inbox[slot].in_use)
        return NULL;
    return &g_aevt->inbox[slot];
}

static DEF_GETTER(aevt_inbox_attr_sender) {
    aevt_inbox_t *in = aevt_obj_inbox(self);
    return val_str(in ? in->sender : "");
}
static DEF_GETTER(aevt_inbox_attr_class) {
    aevt_inbox_t *in = aevt_obj_inbox(self);
    return val_str(in ? in->class4 : "");
}
static DEF_GETTER(aevt_inbox_attr_id) {
    aevt_inbox_t *in = aevt_obj_inbox(self);
    return val_str(in ? in->id4 : "");
}
static DEF_GETTER(aevt_inbox_attr_event) {
    aevt_inbox_t *in = aevt_obj_inbox(self);
    if (!in || in->map.kind != V_MAP)
        return val_map(NULL, 0);
    return value_dup(&in->map);
}
static DEF_GETTER(aevt_inbox_attr_text) {
    aevt_inbox_t *in = aevt_obj_inbox(self);
    return val_str(in && in->text ? in->text : "");
}
static DEF_GETTER(aevt_inbox_attr_error) {
    aevt_inbox_t *in = aevt_obj_inbox(self);
    if (in && val_is_error(&in->map))
        return val_str(val_as_str(&in->map));
    return val_str("");
}

static const member_t aevt_inbox_entry_members[] = {
    {.kind = M_ATTR,
     .name = "sender",
     .doc = "The port the event came from",
     .attr = {.type = V_STRING, .get = aevt_inbox_attr_sender}                                                             },
    {.kind = M_ATTR, .name = "class", .doc = "Event class",        .attr = {.type = V_STRING, .get = aevt_inbox_attr_class}},
    {.kind = M_ATTR, .name = "id",    .doc = "Event ID",           .attr = {.type = V_STRING, .get = aevt_inbox_attr_id}   },
    {.kind = M_ATTR,
     .name = "event",
     .doc = "The decoded event as a map",
     .attr = {.type = V_MAP, .get = aevt_inbox_attr_event}                                                                 },
    {.kind = M_ATTR,
     .name = "text",
     .doc = "The event in text form",
     .attr = {.type = V_STRING, .get = aevt_inbox_attr_text}                                                               },
    {.kind = M_ATTR,
     .name = "error",
     .doc = "Why the event could not be decoded, if it could not",
     .attr = {.type = V_STRING, .get = aevt_inbox_attr_error}                                                              },
};

static const class_desc_t aevt_inbox_entry_class = {
    .name = "aevt_inbox_entry",
    .members = aevt_inbox_entry_members,
    .n_members = ARRAY_LEN(aevt_inbox_entry_members),
};

static struct object *aevt_inbox_get(struct object *self, int index) {
    (void)self;
    if (index < 0 || index >= g_aevt->inbox_count || !g_aevt->inbox[index].in_use)
        return NULL;
    return object_cache_at(&g_host->inbox_entries, index, NULL);
}

static DEF_METHOD(aevt_inbox_method_clear) {
    aevt_inbox_clear();
    return val_none();
}

static const collection_desc_t aevt_inbox_entries = {
    .entry = &aevt_inbox_entry_class, .by_index = {.get = aevt_inbox_get, .slots = AEVT_MAX_INBOX}
};

static const member_t aevt_inbox_members[] = {
    {.kind = M_METHOD,
     .name = "clear",
     .doc = "Forget every event guests have sent us, making room for more",
     .method = {.args = NULL,
                .nargs = 0,
                .result = V_NONE,
                .fn = aevt_inbox_method_clear,
                .ui_flags = MM_DESTRUCTIVE | MM_MUTATE}},
    OBJ_ENTRIES(&aevt_inbox_entries, NULL),
};

static const class_desc_t aevt_inbox_class = {
    .name = "aevt_inbox",
    .doc = "Apple events the guest sent to the host",
    .members = aevt_inbox_members,
    .n_members = ARRAY_LEN(aevt_inbox_members),
};

// --- appletalk.aevt.stats ----------------------------------------------------

// The plugged-in connection's counters (zeros while none is).
static DEF_GETTER(aevt_stats_get) {
    return obj_u64_at(&g_aevt->stats, m);
}

static const member_t aevt_stats_members[] = {
    OBJ_U64_FIELD_WITH(aevt_stats_t, sent, "Events put on the wire", aevt_stats_get),
    OBJ_U64_FIELD_WITH(aevt_stats_t, replied, "Events that came back answered", aevt_stats_get),
    OBJ_U64_FIELD_WITH(aevt_stats_t, errors, "Events that failed", aevt_stats_get),
    OBJ_U64_FIELD_WITH(aevt_stats_t, timeouts, "Events whose instruction budget ran out", aevt_stats_get),
    OBJ_U64_FIELD_WITH(aevt_stats_t, received, "Events guests sent us", aevt_stats_get),
    OBJ_U64_FIELD_WITH(aevt_stats_t, auto_replies, "Automatic replies we sent", aevt_stats_get),
    OBJ_U64_FIELD_WITH(aevt_stats_t, malformed, "Blocks discarded as no high-level event", aevt_stats_get),
    OBJ_U64_FIELD_WITH(aevt_stats_t, dropped, "Events received with the inbox full: answered, not kept",
                       aevt_stats_get),
};

static const class_desc_t aevt_stats_class = {
    .name = "aevt_stats",
    .members = aevt_stats_members,
    .n_members = ARRAY_LEN(aevt_stats_members),
};

// --- appletalk.aevt ----------------------------------------------------------

// The host port is PPC's: these read and change it there.
static DEF_GETTER(aevt_attr_enabled) {
    return val_bool(atalk_ppc_host_port_enabled());
}
static DEF_SETTER(aevt_attr_set_enabled) {
    char err[192] = "";
    if (atalk_ppc_set_host_port(NULL, in.b, err, sizeof(err)) != 0)
        return val_err("cannot change the host program-linking port: %s", err);
    return val_none();
}
static DEF_GETTER(aevt_attr_port_name) {
    return val_str(atalk_ppc_host_port_name());
}
static DEF_SETTER(aevt_attr_set_port_name) {
    char err[192] = "";
    int rc = atalk_ppc_set_host_port(in.s, atalk_ppc_host_port_enabled(), err, sizeof(err));
    value_free(&in);
    if (rc != 0)
        return val_err("cannot rename the host program-linking port: %s", err);
    return val_none();
}
static DEF_GETTER(aevt_attr_auto_reply) {
    return val_str(g_host->auto_reply);
}
static DEF_SETTER(aevt_attr_set_auto_reply) {
    const char *text = in.s ? in.s : "";
    if (text[0]) {
        // Reject a template that does not parse now rather than at delivery.
        char err[192] = "";
        value_t probe = aevt_parse_text(text, err, sizeof(err));
        bool bad = val_is_error(&probe);
        value_free(&probe);
        if (bad) {
            value_free(&in);
            return val_err("that reply template does not parse: %s", err);
        }
    }
    snprintf(g_host->auto_reply, sizeof(g_host->auto_reply), "%s", text);
    value_free(&in);
    return val_none();
}

// Shared tail of send and send_raw: register the event and start it.
static value_t aevt_finish_send(aevt_event_t *ev) {
    aevt_begin(ev);
    if (ev->slot < AEVT_MAX_EVENTS && object_cache_at(&g_host->event_entries, ev->slot, NULL))
        return val_obj(object_cache_at(&g_host->event_entries, ev->slot, NULL));
    return val_none();
}

static DEF_METHOD(aevt_method_send) {
    const char *target = val_as_str(&argv[0]);
    const char *text = val_as_str(&argv[1]);
    if (!target || !*target)
        return val_err("no target port was named");
    if (!text || !*text)
        return val_err("no event was given");

    char err[192] = "";
    value_t request = aevt_parse_text(text, err, sizeof(err));
    if (val_is_error(&request)) {
        value_free(&request);
        return val_err("cannot parse the event: %s", err);
    }
    aevt_event_t *ev = aevt_alloc_event();
    if (!ev) {
        value_free(&request);
        if (g_aevt == &g_no_link)
            return val_err("no machine is connected to the network");
        return val_err("no room for another event on this connection (limit %d)", AEVT_MAX_EVENTS);
    }
    aevt_event_codes(&request, ev->class4, ev->id4);
    ev->request = request;
    ev->text = strdup(text);
    snprintf(ev->target, sizeof(ev->target), "%s", target);

    // Named arguments: timeout is an instruction budget, mode picks whether a
    // reply is expected at all (§8).
    ev->timeout_instr = AEVT_DEFAULT_TIMEOUT_INSTR;
    if (argc > 2 && argv[2].kind != V_NONE) {
        uint64_t budget = val_as_u64(&argv[2], NULL);
        if (budget > 0)
            ev->timeout_instr = budget; // 0 means "wait indefinitely"
        else
            ev->timeout_instr = 0;
    }
    if (argc > 3 && argv[3].kind != V_NONE) {
        const char *tag = val_as_str(&argv[3]);
        if (tag)
            snprintf(ev->tag, sizeof(ev->tag), "%s", tag);
    }
    if (argc > 4 && argv[4].kind != V_NONE) {
        const char *mode = val_as_str(&argv[4]);
        ev->no_reply = (mode && !strcmp(mode, "no_reply"));
    }

    // Ask for a reply the way the Apple Event Manager does: a zero-length
    // `true` attribute in the meta section (§5.3).  Without it the receiving
    // application has no reason to answer, and the event times out even
    // though it was delivered and handled.
    if (!ev->no_reply) {
        value_map_builder_t *flag = val_map_new();
        val_map_put(flag, "type", val_str("true"));
        aevt_set_attr(&ev->request, "repq", val_map_finish(flag));
    }
    return aevt_finish_send(ev);
}

static DEF_METHOD(aevt_method_send_raw) {
    const char *target = val_as_str(&argv[0]);
    if (!target || !*target)
        return val_err("no target port was named");
    if (argv[1].kind != V_BYTES)
        return val_err("send_raw needs the flattened event as bytes");

    // A pre-flattened payload still has to name its class and ID, so it is
    // decoded once here; a stream we cannot read is refused up front.
    const char *class4 = val_as_str(&argv[2]);
    const char *id4 = val_as_str(&argv[3]);
    if (!class4 || !id4)
        return val_err("send_raw needs the event class and ID");

    value_t request = aevt_decode(class4, id4, argv[1].bytes.p, (int)argv[1].bytes.n);
    if (val_is_error(&request)) {
        // Sending bytes we cannot parse is the point of the golden path, so
        // keep going, but remember them verbatim.
        value_free(&request);
        request = val_none();
    }
    aevt_event_t *ev = aevt_alloc_event();
    if (!ev) {
        value_free(&request);
        if (g_aevt == &g_no_link)
            return val_err("no machine is connected to the network");
        return val_err("no room for another event on this connection (limit %d)", AEVT_MAX_EVENTS);
    }
    snprintf(ev->class4, sizeof(ev->class4), "%-4.4s", class4);
    snprintf(ev->id4, sizeof(ev->id4), "%-4.4s", id4);
    ev->request = request;
    ev->text = strdup("(raw)");
    ev->timeout_instr = AEVT_DEFAULT_TIMEOUT_INSTR;
    snprintf(ev->target, sizeof(ev->target), "%s", target);

    char err[192] = "";
    ppc_session_t *s = aevt_session_for(ev->target, err, sizeof(err));
    if (!s) {
        aevt_fail(ev, err);
        return val_obj(object_cache_at(&g_host->event_entries, ev->slot, NULL));
    }
    ev->session = s;
    if (atalk_ppc_session_state(s) == PPC_SESSION_OPEN) {
        if (!aevt_write_event(s, ev->class4, ev->id4, ev->return_id, false, argv[1].bytes.p, (int)argv[1].bytes.n, err,
                              sizeof(err)))
            aevt_fail(ev, err);
        else {
            ev->state = AEVT_STATE_SENT;
            ev->sent_at_instr = cpu_instr_count();
            g_aevt->stats.sent++;
        }
    } else {
        aevt_fail(ev, "no session to that port is open yet; browse and retry");
    }
    return val_obj(object_cache_at(&g_host->event_entries, ev->slot, NULL));
}

static const value_t aevt_def_timeout = {.kind = V_UINT, .width = 8, .u = AEVT_DEFAULT_TIMEOUT_INSTR};
static const value_t aevt_def_mode = {.kind = V_STRING, .s = (char *)"wait"};

static const arg_decl_t aevt_send_args[] = {
    {.name = "target",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_NONEMPTY,
     .doc = "Program-linking port name, as `ppc.ports` shows it"},
    {.name = "event",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_NONEMPTY,
     .doc = "The event in text form, e.g. aevt/odoc{'----':[…]}"},
    {.name = "timeout",
     .kind = V_UINT,
     .width = 8,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &aevt_def_timeout,
     .doc = "Reply budget in guest instructions"},
    {.name = "tag",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Lookup key, so events[\"name\"] finds this event"},
    {.name = "mode",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &aevt_def_mode,
     .doc = "\"wait\" or \"no_reply\""},
};

static const arg_decl_t aevt_send_raw_args[] = {
    {.name = "target", .kind = V_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "Program-linking port name"},
    {.name = "stream", .kind = V_BYTES, .doc = "A pre-flattened event stream"},
    {.name = "class", .kind = V_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "Event class"},
    {.name = "id", .kind = V_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "Event ID"},
};

static const member_t aevt_members[] = {
    {.kind = M_ATTR,
     .name = "enabled",
     .doc = "Advertise the host program-linking port and accept sessions",
     .attr = {.type = V_BOOL, .get = aevt_attr_enabled, .set = aevt_attr_set_enabled}},
    {.kind = M_ATTR,
     .name = "port_name",
     .doc = "NBP object name of the host port guests see in their PPC browser",
     .attr = {.type = V_STRING,
              .validation_flags = OBJ_ARG_NONEMPTY,
              .get = aevt_attr_port_name,
              .set = aevt_attr_set_port_name}},
    {.kind = M_ATTR,
     .name = "auto_reply",
     .doc = "Text-form reply sent for each inbox event; empty means a plain noErr answer",
     .attr = {.type = V_STRING, .get = aevt_attr_auto_reply, .set = aevt_attr_set_auto_reply}},
    {.kind = M_METHOD,
     .name = "send",
     .doc = "Send an Apple event to a guest application; returns the event object, does not wait",
     .method = {.args = aevt_send_args,
                .nargs = ARRAY_LEN(aevt_send_args),
                .result = V_OBJECT,
                .fn = aevt_method_send,
                .ui_flags = MM_MUTATE}},
    {.kind = M_METHOD,
     .name = "send_raw",
     .flags = M_CAT_ADVANCED,
     .doc = "Send a pre-flattened event stream verbatim (golden and fuzz path)",
     .method = {.args = aevt_send_raw_args,
                .nargs = ARRAY_LEN(aevt_send_raw_args),
                .result = V_OBJECT,
                .fn = aevt_method_send_raw,
                .ui_flags = MM_MUTATE}},
};

static const class_desc_t aevt_class = {
    .name = "aevt",
    .doc = "Apple events over AppleTalk: send to guest ports, receive replies and events",
    .members = aevt_members,
    .n_members = ARRAY_LEN(aevt_members),
};

void atalk_aevt_install_objects(struct object *parent) {
    struct object *aevt = object_new(&aevt_class, NULL, "aevt");
    if (!aevt)
        return;
    object_set_label(aevt, "Apple Events");
    object_attach(parent, aevt);

    struct object *events = object_new(&aevt_events_class, NULL, "events");
    if (events)
        object_attach(aevt, events);
    struct object *inbox = object_new(&aevt_inbox_class, NULL, "inbox");
    if (inbox)
        object_attach(aevt, inbox);
    struct object *stats = object_new(&aevt_stats_class, NULL, "stats");
    if (stats) {
        object_set_category(stats, M_CAT_ADVANCED);
        object_attach(aevt, stats);
    }

    // The collection entry objects, made on first use.
    g_host->event_entries = (object_cache_t)OBJECT_CACHE(&aevt_event_class, NULL);
    g_host->inbox_entries = (object_cache_t)OBJECT_CACHE(&aevt_inbox_entry_class, NULL);
    object_cache_set_parent(&g_host->event_entries, events);
    object_cache_set_parent(&g_host->inbox_entries, inbox);
}
