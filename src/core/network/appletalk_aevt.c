// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_aevt.c
// The `appletalk.aevt` surface: send Apple events to guest applications, take
// delivery of the ones they send us, and publish both as object-model state.
//
// Coding reference: docs/core/network/ppc_appleevents.md §5 (high-level event
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

LOG_USE_CATEGORY_NAME("ppc");

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

// ============================================================================
// Constants and Macros
// ============================================================================

#define AEVT_MAX_EVENTS 64 // events remembered per run (append-only, §8)
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

static const char *const AEVT_STATE_NAMES[] = {"queued", "sent", "replied", "error", "timeout"};
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

static aevt_event_t g_events[AEVT_MAX_EVENTS];
static int g_event_count; // append-only high-water mark
static aevt_inbox_t g_inbox[AEVT_MAX_INBOX];
static int g_inbox_count;

static bool g_enabled = true;
static char g_port_name[33] = "gs-host";
static char g_auto_reply[256] = "";

static uint32_t g_next_return_id = 1;

// Counters published as `appletalk.aevt.stats`.
typedef struct {
    uint64_t sent;
    uint64_t replied;
    uint64_t errors;
    uint64_t timeouts;
    uint64_t received;
    uint64_t auto_replies;
} aevt_stats_t;

static aevt_stats_t g_stats;

// ============================================================================
// Forward declarations
// ============================================================================

static void aevt_flush_session(ppc_session_t *s);
static void aevt_settle(aevt_event_t *ev);
static bool aevt_dispatch(aevt_event_t *ev, char *err, size_t err_len);

// ============================================================================
// Operations — small helpers
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

static uint32_t fourcc_of(const char *s) {
    uint8_t b[4];
    for (int i = 0; i < 4; i++)
        b[i] = (s && s[i]) ? (uint8_t)s[i] : (uint8_t)' ';
    return get32(b);
}

static void fourcc_str(uint32_t v, char out[5]) {
    out[0] = (char)(v >> 24);
    out[1] = (char)(v >> 16);
    out[2] = (char)(v >> 8);
    out[3] = (char)v;
    out[4] = '\0';
}

// ============================================================================
// Operations — the event table
// ============================================================================

static aevt_event_t *aevt_alloc_event(void) {
    // Events are append-only for the life of the run so the V_OBJECT a send
    // returns stays valid in a `let` binding (§8).
    if (g_event_count >= AEVT_MAX_EVENTS)
        return NULL;
    aevt_event_t *ev = &g_events[g_event_count];
    memset(ev, 0, sizeof(*ev));
    ev->in_use = true;
    ev->slot = g_event_count++;
    ev->return_id = g_next_return_id++;
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
    g_stats.errors++;
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
            g_stats.timeouts++;
        }
    }
    return ev->state;
}

static aevt_event_t *aevt_find_by_return_id(uint32_t return_id) {
    for (int i = 0; i < g_event_count; i++) {
        if (g_events[i].in_use && g_events[i].return_id == return_id)
            return &g_events[i];
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
    put16(&msg[0], AEVT_HLE_HEADER_SIZE);
    put16(&msg[2], AEVT_HLE_VERSION);
    put32(&msg[4], 0); // reserved
    // The embedded EventRecord is a header, not an event (§5.1).
    put16(&msg[8], AEVT_HLE_WHAT);
    put32(&msg[10], fourcc_of(class4)); // message = event class
    put32(&msg[14], 0); // when: the receiver stamps it
    put32(&msg[18], fourcc_of(id4)); // where = event ID
    put16(&msg[22], is_reply ? AEVT_MODIFIER_REPLY : 0);
    put32(&msg[24], return_id);
    put32(&msg[28], 0); // posting options
    put32(&msg[32], (uint32_t)stream_len);
    if (stream_len > 0)
        memcpy(&msg[AEVT_HLE_HEADER_SIZE], stream, (size_t)stream_len);

    if (atalk_ppc_send_block(s, fourcc_of(class4), fourcc_of(id4), return_id, msg, AEVT_HLE_HEADER_SIZE + stream_len) !=
        0) {
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
    g_stats.sent++;
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
    for (int i = 0; i < g_event_count; i++) {
        aevt_event_t *ev = &g_events[i];
        if (ev->in_use && ev->session == s && (ev->state == AEVT_STATE_QUEUED || ev->state == AEVT_STATE_SENT))
            aevt_fail(ev, reason);
    }
}

static void aevt_session_closed(void *ctx, ppc_session_t *s) {
    (void)ctx;
    for (int i = 0; i < g_event_count; i++) {
        aevt_event_t *ev = &g_events[i];
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
        LOG(3, "AE: a %d-byte block is too short to be a high-level event", len);
        return;
    }
    uint16_t header_len = get16(&payload[0]);
    uint16_t version = get16(&payload[2]);
    if (header_len < AEVT_HLE_HEADER_SIZE || header_len > len) {
        LOG(3, "AE: high-level event header length %u is not usable", (unsigned)header_len);
        return;
    }
    if (version > AEVT_HLE_VERSION)
        LOG(3, "AE: high-level event version %u is newer than we know", (unsigned)version);

    uint16_t modifiers = get16(&payload[22]);
    uint32_t return_id = get32(&payload[24]);
    uint32_t msg_len = get32(&payload[32]);
    // The class and ID live in the EventRecord overlay; the block header
    // carries them too, and we trust the overlay (§5.1).
    char class4[5], id4[5];
    fourcc_str(get32(&payload[10]), class4);
    fourcc_str(get32(&payload[18]), id4);
    if (class4[0] == '\0')
        fourcc_str(creator, class4);
    if (id4[0] == '\0')
        fourcc_str(type, id4);

    const uint8_t *stream = payload + header_len;
    int avail = len - header_len;
    if (msg_len > (uint32_t)avail) {
        LOG(3, "AE: the event claims %u bytes but the block holds %d", (unsigned)msg_len, avail);
        return;
    }

    atalk_aevt_deliver((uint16_t)atalk_ppc_session_id(s), atalk_ppc_session_port(s), class4, id4, return_id,
                       (modifiers & AEVT_MODIFIER_REPLY) != 0, stream, (int)msg_len);
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
    if (g_auto_reply[0]) {
        reply = aevt_parse_text(g_auto_reply, err, sizeof(err));
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
        g_stats.auto_replies++;
    else
        LOG(2, "AE: the auto-reply could not be sent — %s", err);
    value_free(&reply);
}

void atalk_aevt_deliver(uint16_t session_id, const char *sender, const char *class4, const char *id4,
                        uint32_t return_id, bool is_reply, const uint8_t *stream, int len) {
    (void)session_id;
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
        g_stats.replied++;
        LOG(3, "AE: event %u replied (errn=%lld)", (unsigned)return_id, (long long)ev->errn);
        return;
    }

    // Not a reply: this is an event the guest sent us.
    g_stats.received++;
    if (g_inbox_count >= AEVT_MAX_INBOX) {
        LOG(2, "AE: the inbox is full; dropping %s/%s", class4, id4);
        value_free(&map);
        return;
    }
    aevt_inbox_t *in = &g_inbox[g_inbox_count];
    memset(in, 0, sizeof(*in));
    in->in_use = true;
    in->slot = g_inbox_count++;
    snprintf(in->sender, sizeof(in->sender), "%s", sender ? sender : "");
    snprintf(in->class4, sizeof(in->class4), "%s", class4 ? class4 : "");
    snprintf(in->id4, sizeof(in->id4), "%s", id4 ? id4 : "");
    in->return_id = return_id;
    in->map = map;
    in->text = val_is_error(&map) ? NULL : aevt_render_text(&map);
    LOG(3, "AE: received %s/%s from '%s'", in->class4, in->id4, in->sender);

    // Answer on the session it came in on, so the sender's AESend completes.
    for (int i = 0; i < atalk_ppc_session_slot_max(); i++) {
        ppc_session_t *s = atalk_ppc_session_at(i);
        if (s && atalk_ppc_session_id(s) == session_id) {
            aevt_send_auto_reply(s, return_id);
            break;
        }
    }
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
    for (int i = 0; i < g_event_count; i++)
        if (g_events[i].in_use && g_events[i].session == s)
            g_events[i].session = NULL;
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
    for (int i = 0; i < g_event_count; i++) {
        aevt_event_t *ev = &g_events[i];
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

void atalk_aevt_reset_transient_state(void) {
    for (int i = 0; i < AEVT_MAX_EVENTS; i++)
        if (g_events[i].in_use)
            aevt_event_free_contents(&g_events[i]);
    g_event_count = 0;
    for (int i = 0; i < AEVT_MAX_INBOX; i++) {
        if (!g_inbox[i].in_use)
            continue;
        value_free(&g_inbox[i].map);
        free(g_inbox[i].text);
        memset(&g_inbox[i], 0, sizeof(g_inbox[i]));
    }
    g_inbox_count = 0;
    g_next_return_id = 1;
    memset(&g_stats, 0, sizeof(g_stats));
}

void atalk_aevt_init(void) {
    atalk_aevt_reset_transient_state();
    g_enabled = true;
    snprintf(g_port_name, sizeof(g_port_name), "gs-host");
    g_auto_reply[0] = '\0';
    atalk_ppc_set_inbound_client(&g_session_client, NULL);
    char err[192] = "";
    if (atalk_ppc_set_host_port(g_port_name, g_enabled, err, sizeof(err)) != 0)
        LOG(2, "AE: the host port could not be published — %s", err);
}

void atalk_aevt_shutdown(void) {
    atalk_aevt_reset_transient_state();
    atalk_ppc_set_inbound_client(NULL, NULL);
}

void atalk_aevt_get_config(atalk_aevt_config_t *out) {
    if (!out)
        return;
    out->enabled = g_enabled;
    snprintf(out->port_name, sizeof(out->port_name), "%s", g_port_name);
    snprintf(out->auto_reply, sizeof(out->auto_reply), "%s", g_auto_reply);
}

void atalk_aevt_set_config(const atalk_aevt_config_t *in) {
    if (!in)
        return;
    g_enabled = in->enabled;
    snprintf(g_port_name, sizeof(g_port_name), "%s", in->port_name);
    snprintf(g_auto_reply, sizeof(g_auto_reply), "%s", in->auto_reply);
    char err[192] = "";
    if (atalk_ppc_set_host_port(g_port_name, g_enabled, err, sizeof(err)) != 0)
        LOG(2, "AE: the host port could not be republished — %s", err);
}

// ============================================================================
// Object model — `appletalk.aevt`
// ============================================================================

static struct object *g_aevt_object;
static struct object *g_aevt_events_object;
static struct object *g_aevt_inbox_object;
static struct object *g_aevt_stats_object;

typedef struct {
    int slot;
} aevt_slot_data_t;

static aevt_slot_data_t g_aevt_event_data[AEVT_MAX_EVENTS];
static struct object *g_aevt_event_objs[AEVT_MAX_EVENTS];
static aevt_slot_data_t g_aevt_inbox_data[AEVT_MAX_INBOX];
static struct object *g_aevt_inbox_objs[AEVT_MAX_INBOX];

extern const class_desc_t aevt_class;
extern const class_desc_t aevt_events_class;
extern const class_desc_t aevt_event_class;
extern const class_desc_t aevt_inbox_class;
extern const class_desc_t aevt_inbox_entry_class;
extern const class_desc_t aevt_stats_class;

static int aevt_obj_slot(struct object *self) {
    const aevt_slot_data_t *d = (const aevt_slot_data_t *)object_data(self);
    return d ? d->slot : -1;
}

static aevt_event_t *aevt_obj_event(struct object *self) {
    int slot = aevt_obj_slot(self);
    if (slot < 0 || slot >= g_event_count || !g_events[slot].in_use)
        return NULL;
    return &g_events[slot];
}

// --- appletalk.aevt.events[i] ------------------------------------------------

static value_t aevt_event_attr_state(struct object *self, const member_t *m) {
    (void)m;
    aevt_event_t *ev = aevt_obj_event(self);
    int st = ev ? (int)aevt_effective_state(ev) : 0;
    return val_enum(st, AEVT_STATE_NAMES, AEVT_STATE_COUNT);
}
static value_t aevt_event_attr_target(struct object *self, const member_t *m) {
    (void)m;
    aevt_event_t *ev = aevt_obj_event(self);
    return val_str(ev ? ev->target : "");
}
static value_t aevt_event_attr_tag(struct object *self, const member_t *m) {
    (void)m;
    aevt_event_t *ev = aevt_obj_event(self);
    return val_str(ev ? ev->tag : "");
}
static value_t aevt_event_attr_text(struct object *self, const member_t *m) {
    (void)m;
    aevt_event_t *ev = aevt_obj_event(self);
    return val_str(ev && ev->text ? ev->text : "");
}
static value_t aevt_event_attr_class(struct object *self, const member_t *m) {
    (void)m;
    aevt_event_t *ev = aevt_obj_event(self);
    return val_str(ev ? ev->class4 : "");
}
static value_t aevt_event_attr_id(struct object *self, const member_t *m) {
    (void)m;
    aevt_event_t *ev = aevt_obj_event(self);
    return val_str(ev ? ev->id4 : "");
}
static value_t aevt_event_attr_reply(struct object *self, const member_t *m) {
    (void)m;
    aevt_event_t *ev = aevt_obj_event(self);
    if (!ev || ev->reply.kind != V_MAP)
        return val_map(NULL, 0);
    return value_copy(&ev->reply);
}
static value_t aevt_event_attr_request(struct object *self, const member_t *m) {
    (void)m;
    aevt_event_t *ev = aevt_obj_event(self);
    if (!ev || ev->request.kind != V_MAP)
        return val_map(NULL, 0);
    return value_copy(&ev->request);
}
static value_t aevt_event_attr_errn(struct object *self, const member_t *m) {
    (void)m;
    aevt_event_t *ev = aevt_obj_event(self);
    return val_int(ev ? ev->errn : 0);
}
static value_t aevt_event_attr_error(struct object *self, const member_t *m) {
    (void)m;
    aevt_event_t *ev = aevt_obj_event(self);
    if (ev)
        aevt_effective_state(ev); // a lazy timeout produces the message
    return val_str(ev && ev->error ? ev->error : "");
}
static value_t aevt_event_attr_return_id(struct object *self, const member_t *m) {
    (void)m;
    aevt_event_t *ev = aevt_obj_event(self);
    return val_uint(4, ev ? ev->return_id : 0);
}

static const member_t aevt_event_members[] = {
    {.kind = M_ATTR,
     .name = "state",
     .doc = "queued, sent, replied, error or timeout",
     .flags = VAL_RO,
     .attr = {.type = V_ENUM, .enum_values = AEVT_STATE_NAMES, .get = aevt_event_attr_state}},
    {.kind = M_ATTR,
     .name = "target",
     .doc = "The program-linking port this event was addressed to",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = aevt_event_attr_target}                              },
    {.kind = M_ATTR,
     .name = "tag",
     .doc = "Lookup key given at send time, if any",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = aevt_event_attr_tag}                                 },
    {.kind = M_ATTR,
     .name = "class",
     .doc = "Event class",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = aevt_event_attr_class}                               },
    {.kind = M_ATTR,
     .name = "id",
     .doc = "Event ID",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = aevt_event_attr_id}                                  },
    {.kind = M_ATTR,
     .name = "text",
     .doc = "The request in text form",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = aevt_event_attr_text}                                },
    {.kind = M_ATTR,
     .name = "request",
     .doc = "The request as a map",
     .flags = VAL_RO,
     .attr = {.type = V_MAP, .get = aevt_event_attr_request}                                },
    {.kind = M_ATTR,
     .name = "reply",
     .doc = "The reply as a map; empty until one arrives",
     .flags = VAL_RO,
     .attr = {.type = V_MAP, .get = aevt_event_attr_reply}                                  },
    {.kind = M_ATTR,
     .name = "errn",
     .doc = "keyErrorNumber from the reply; 0 means success",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = aevt_event_attr_errn}                                   },
    {.kind = M_ATTR,
     .name = "error",
     .doc = "Why the event failed, when it did",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = aevt_event_attr_error}                               },
    {.kind = M_ATTR,
     .name = "return_id",
     .doc = "The return ID that correlates the reply",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .width = 4, .get = aevt_event_attr_return_id}                 },
};

const class_desc_t aevt_event_class = {
    .name = "aevt_event",
    .members = aevt_event_members,
    .n_members = ARRAY_LEN(aevt_event_members),
};

// --- appletalk.aevt.events ---------------------------------------------------

static struct object *aevt_events_get(struct object *self, int index) {
    (void)self;
    if (index < 0 || index >= g_event_count || !g_events[index].in_use)
        return NULL;
    return g_aevt_event_objs[index];
}
static int aevt_events_count(struct object *self) {
    (void)self;
    return g_event_count;
}
static int aevt_events_next(struct object *self, int prev) {
    (void)self;
    int next = prev + 1;
    return (next < g_event_count) ? next : -1;
}
// Name lookup resolves the `tag:` given at send time (§8).
static struct object *aevt_events_lookup(struct object *self, const char *name) {
    (void)self;
    for (int i = 0; i < g_event_count; i++)
        if (g_events[i].in_use && g_events[i].tag[0] && !strcmp(g_events[i].tag, name))
            return g_aevt_event_objs[i];
    return NULL;
}
static value_t aevt_events_attr_count(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, (uint64_t)g_event_count);
}

static const member_t aevt_events_members[] = {
    {.kind = M_ATTR,
     .name = "count",
     .doc = "Events sent this run",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .width = 4, .get = aevt_events_attr_count}},
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &aevt_event_class,
               .indexed = true,
               .get = aevt_events_get,
               .count = aevt_events_count,
               .next = aevt_events_next,
               .lookup = aevt_events_lookup}},
};

const class_desc_t aevt_events_class = {
    .name = "aevt_events",
    .members = aevt_events_members,
    .n_members = ARRAY_LEN(aevt_events_members),
};

// --- appletalk.aevt.inbox[i] -------------------------------------------------

static aevt_inbox_t *aevt_obj_inbox(struct object *self) {
    int slot = aevt_obj_slot(self);
    if (slot < 0 || slot >= g_inbox_count || !g_inbox[slot].in_use)
        return NULL;
    return &g_inbox[slot];
}

static value_t aevt_inbox_attr_sender(struct object *self, const member_t *m) {
    (void)m;
    aevt_inbox_t *in = aevt_obj_inbox(self);
    return val_str(in ? in->sender : "");
}
static value_t aevt_inbox_attr_class(struct object *self, const member_t *m) {
    (void)m;
    aevt_inbox_t *in = aevt_obj_inbox(self);
    return val_str(in ? in->class4 : "");
}
static value_t aevt_inbox_attr_id(struct object *self, const member_t *m) {
    (void)m;
    aevt_inbox_t *in = aevt_obj_inbox(self);
    return val_str(in ? in->id4 : "");
}
static value_t aevt_inbox_attr_event(struct object *self, const member_t *m) {
    (void)m;
    aevt_inbox_t *in = aevt_obj_inbox(self);
    if (!in || in->map.kind != V_MAP)
        return val_map(NULL, 0);
    return value_copy(&in->map);
}
static value_t aevt_inbox_attr_text(struct object *self, const member_t *m) {
    (void)m;
    aevt_inbox_t *in = aevt_obj_inbox(self);
    return val_str(in && in->text ? in->text : "");
}
static value_t aevt_inbox_attr_error(struct object *self, const member_t *m) {
    (void)m;
    aevt_inbox_t *in = aevt_obj_inbox(self);
    if (in && val_is_error(&in->map))
        return val_str(val_as_str(&in->map));
    return val_str("");
}

static const member_t aevt_inbox_entry_members[] = {
    {.kind = M_ATTR,
     .name = "sender",
     .doc = "The port the event came from",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = aevt_inbox_attr_sender}},
    {.kind = M_ATTR,
     .name = "class",
     .doc = "Event class",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = aevt_inbox_attr_class} },
    {.kind = M_ATTR,
     .name = "id",
     .doc = "Event ID",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = aevt_inbox_attr_id}    },
    {.kind = M_ATTR,
     .name = "event",
     .doc = "The decoded event as a map",
     .flags = VAL_RO,
     .attr = {.type = V_MAP, .get = aevt_inbox_attr_event}    },
    {.kind = M_ATTR,
     .name = "text",
     .doc = "The event in text form",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = aevt_inbox_attr_text}  },
    {.kind = M_ATTR,
     .name = "error",
     .doc = "Why the event could not be decoded, if it could not",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = aevt_inbox_attr_error} },
};

const class_desc_t aevt_inbox_entry_class = {
    .name = "aevt_inbox_entry",
    .members = aevt_inbox_entry_members,
    .n_members = ARRAY_LEN(aevt_inbox_entry_members),
};

static struct object *aevt_inbox_get(struct object *self, int index) {
    (void)self;
    if (index < 0 || index >= g_inbox_count || !g_inbox[index].in_use)
        return NULL;
    return g_aevt_inbox_objs[index];
}
static int aevt_inbox_count_cb(struct object *self) {
    (void)self;
    return g_inbox_count;
}
static int aevt_inbox_next(struct object *self, int prev) {
    (void)self;
    int next = prev + 1;
    return (next < g_inbox_count) ? next : -1;
}
static value_t aevt_inbox_attr_count(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, (uint64_t)g_inbox_count);
}

static const member_t aevt_inbox_members[] = {
    {.kind = M_ATTR,
     .name = "count",
     .doc = "Events guests have sent us this run",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .width = 4, .get = aevt_inbox_attr_count}},
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &aevt_inbox_entry_class,
               .indexed = true,
               .get = aevt_inbox_get,
               .count = aevt_inbox_count_cb,
               .next = aevt_inbox_next}},
};

const class_desc_t aevt_inbox_class = {
    .name = "aevt_inbox",
    .members = aevt_inbox_members,
    .n_members = ARRAY_LEN(aevt_inbox_members),
};

// --- appletalk.aevt.stats ----------------------------------------------------

static value_t aevt_stats_attr(struct object *self, const member_t *m) {
    (void)self;
    size_t offset = (size_t)(uintptr_t)m->attr.user_data;
    return val_uint(8, *(const uint64_t *)((const uint8_t *)&g_stats + offset));
}

#define AEVT_STAT_MEMBER(field, doc_text)                                                                              \
    {                                                                                                                  \
        .kind = M_ATTR, .name = #field, .doc = doc_text, .flags = VAL_RO, .attr = {                                    \
            .type = V_UINT,                                                                                            \
            .width = 8,                                                                                                \
            .get = aevt_stats_attr,                                                                                    \
            .user_data = (const void *)(uintptr_t)offsetof(aevt_stats_t, field)                                        \
        }                                                                                                              \
    }

static const member_t aevt_stats_members[] = {
    AEVT_STAT_MEMBER(sent, "Events put on the wire"),
    AEVT_STAT_MEMBER(replied, "Events that came back answered"),
    AEVT_STAT_MEMBER(errors, "Events that failed"),
    AEVT_STAT_MEMBER(timeouts, "Events whose instruction budget ran out"),
    AEVT_STAT_MEMBER(received, "Events guests sent us"),
    AEVT_STAT_MEMBER(auto_replies, "Automatic replies we sent"),
};

const class_desc_t aevt_stats_class = {
    .name = "aevt_stats",
    .members = aevt_stats_members,
    .n_members = ARRAY_LEN(aevt_stats_members),
};

// --- appletalk.aevt ----------------------------------------------------------

static value_t aevt_attr_enabled(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_bool(g_enabled);
}
static value_t aevt_attr_set_enabled(struct object *self, const member_t *m, value_t in) {
    (void)self;
    (void)m;
    char err[192] = "";
    if (atalk_ppc_set_host_port(g_port_name, in.b, err, sizeof(err)) != 0)
        return val_err("cannot change the host program-linking port: %s", err);
    g_enabled = in.b;
    return val_none();
}
static value_t aevt_attr_port_name(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_str(g_port_name);
}
static value_t aevt_attr_set_port_name(struct object *self, const member_t *m, value_t in) {
    (void)self;
    (void)m;
    char err[192] = "";
    if (atalk_ppc_set_host_port(in.s, g_enabled, err, sizeof(err)) != 0)
        return val_err("cannot rename the host program-linking port: %s", err);
    snprintf(g_port_name, sizeof(g_port_name), "%s", in.s);
    return val_none();
}
static value_t aevt_attr_auto_reply(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_str(g_auto_reply);
}
static value_t aevt_attr_set_auto_reply(struct object *self, const member_t *m, value_t in) {
    (void)self;
    (void)m;
    const char *text = in.s ? in.s : "";
    if (text[0]) {
        // Reject a template that does not parse now rather than at delivery.
        char err[192] = "";
        value_t probe = aevt_parse_text(text, err, sizeof(err));
        bool bad = val_is_error(&probe);
        value_free(&probe);
        if (bad)
            return val_err("that reply template does not parse: %s", err);
    }
    snprintf(g_auto_reply, sizeof(g_auto_reply), "%s", text);
    return val_none();
}

// Shared tail of send and send_raw: register the event and start it.
static value_t aevt_finish_send(aevt_event_t *ev) {
    aevt_begin(ev);
    if (ev->slot < AEVT_MAX_EVENTS && g_aevt_event_objs[ev->slot])
        return val_obj(g_aevt_event_objs[ev->slot]);
    return val_none();
}

static value_t aevt_method_send(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
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
        return val_err("no room for another event this run (limit %d)", AEVT_MAX_EVENTS);
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

static value_t aevt_method_send_raw(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
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
        return val_err("no room for another event this run (limit %d)", AEVT_MAX_EVENTS);
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
        return val_obj(g_aevt_event_objs[ev->slot]);
    }
    ev->session = s;
    if (atalk_ppc_session_state(s) == PPC_SESSION_OPEN) {
        if (!aevt_write_event(s, ev->class4, ev->id4, ev->return_id, false, argv[1].bytes.p, (int)argv[1].bytes.n, err,
                              sizeof(err)))
            aevt_fail(ev, err);
        else {
            ev->state = AEVT_STATE_SENT;
            ev->sent_at_instr = cpu_instr_count();
            g_stats.sent++;
        }
    } else {
        aevt_fail(ev, "no session to that port is open yet; browse and retry");
    }
    return val_obj(g_aevt_event_objs[ev->slot]);
}

// Interior optional slots need defaults, or the named-argument binder's
// V_NONE holes are reported as missing arguments when a later slot is named
// (the same trap debug.logpoints.add documents).
static const value_t aevt_def_timeout = {.kind = V_UINT, .width = 8, .u = AEVT_DEFAULT_TIMEOUT_INSTR};
static const value_t aevt_def_tag = {.kind = V_STRING, .s = (char *)""};
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
     .doc = "Reply budget in guest instructions (default 20 million)"},
    {.name = "tag",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &aevt_def_tag,
     .doc = "Lookup key, so events[\"name\"] finds this event"},
    {.name = "mode",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &aevt_def_mode,
     .doc = "\"wait\" (default) or \"no_reply\""},
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
     .attr = {.type = V_BOOL, .get = aevt_attr_enabled, .set = aevt_attr_set_enabled}        },
    {.kind = M_ATTR,
     .name = "port_name",
     .doc = "NBP object name of the host port guests see in their PPC browser",
     .attr = {.type = V_STRING,
              .validation_flags = OBJ_ARG_NONEMPTY,
              .get = aevt_attr_port_name,
              .set = aevt_attr_set_port_name}                                                },
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
                .ui_flags = MM_MUTATE}                                                       },
    {.kind = M_METHOD,
     .name = "send_raw",
     .doc = "Send a pre-flattened event stream verbatim (golden and fuzz path)",
     .method = {.args = aevt_send_raw_args,
                .nargs = ARRAY_LEN(aevt_send_raw_args),
                .result = V_OBJECT,
                .fn = aevt_method_send_raw,
                .ui_flags = MM_MUTATE}                                                       },
};

const class_desc_t aevt_class = {
    .name = "aevt",
    .members = aevt_members,
    .n_members = ARRAY_LEN(aevt_members),
};

void atalk_aevt_install_objects(struct object *parent) {
    if (!parent || g_aevt_object)
        return;
    g_aevt_object = object_new(&aevt_class, NULL, "aevt");
    if (!g_aevt_object)
        return;
    object_set_label(g_aevt_object, "Apple Events");
    object_attach(parent, g_aevt_object);

    g_aevt_events_object = object_new(&aevt_events_class, NULL, "events");
    if (g_aevt_events_object)
        object_attach(g_aevt_object, g_aevt_events_object);
    g_aevt_inbox_object = object_new(&aevt_inbox_class, NULL, "inbox");
    if (g_aevt_inbox_object)
        object_attach(g_aevt_object, g_aevt_inbox_object);
    g_aevt_stats_object = object_new(&aevt_stats_class, NULL, "stats");
    if (g_aevt_stats_object) {
        object_set_category(g_aevt_stats_object, M_CAT_ADVANCED);
        object_attach(g_aevt_object, g_aevt_stats_object);
    }

    for (int i = 0; i < AEVT_MAX_EVENTS; i++) {
        g_aevt_event_data[i].slot = i;
        g_aevt_event_objs[i] = object_new(&aevt_event_class, &g_aevt_event_data[i], NULL);
    }
    for (int i = 0; i < AEVT_MAX_INBOX; i++) {
        g_aevt_inbox_data[i].slot = i;
        g_aevt_inbox_objs[i] = object_new(&aevt_inbox_entry_class, &g_aevt_inbox_data[i], NULL);
    }
}

void atalk_aevt_remove_objects(void) {
    for (int i = 0; i < AEVT_MAX_EVENTS; i++) {
        if (g_aevt_event_objs[i])
            object_delete(g_aevt_event_objs[i]);
        g_aevt_event_objs[i] = NULL;
    }
    for (int i = 0; i < AEVT_MAX_INBOX; i++) {
        if (g_aevt_inbox_objs[i])
            object_delete(g_aevt_inbox_objs[i]);
        g_aevt_inbox_objs[i] = NULL;
    }
    struct object **nodes[] = {&g_aevt_events_object, &g_aevt_inbox_object, &g_aevt_stats_object, &g_aevt_object};
    for (int i = 0; i < ARRAY_LEN(nodes); i++) {
        if (!*nodes[i])
            continue;
        object_detach(*nodes[i]);
        object_delete(*nodes[i]);
        *nodes[i] = NULL;
    }
}
