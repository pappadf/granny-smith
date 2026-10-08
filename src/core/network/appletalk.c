// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk.c
// AppleTalk networking protocol stack implementation.

// ============================================================================
// Includes
// ============================================================================

#include "appletalk.h"

#include "afp_server.h"
#include "appletalk_adsp.h"
#include "appletalk_aevt.h"
#include "appletalk_asp.h"
#include "appletalk_internal.h"
#include "appletalk_ppc.h"
#include "atalk_id.h"
#include "checkpoint.h"
#include "common.h"
#include "gs_assert.h"
#include "laserwriter_job.h"
#include "log.h"
#include "macroman.h"
#include "object.h"
#include "scc.h"
#include "scheduler.h"
#include "shell.h"
#include "system.h"
#include "value.h"
#include "worker_thread.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and Macros
// ============================================================================

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

// LLAP's data field holds at most 600 bytes (Inside AppleTalk, LLAP chapter), the
// link's own limit: the longest DDP datagram, an extended header and a full
// 586-byte payload, fits it with a byte to spare.
#define LLAP_DATA_MAX_SIZE 600
static_assert(DDP_EXTENDED_HEADER_SIZE + DDP_MAX_DATA_SIZE <= LLAP_DATA_MAX_SIZE,
              "a DDP datagram must fit one LLAP frame");

#define LLAP_DDP_SHORT    0x01
#define LLAP_DDP_EXTENDED 0x02

#define LLAP_ENQ 0x81
#define LLAP_ACK 0x82
#define LLAP_RTS 0x84
#define LLAP_CTS 0x85

// One directed data frame parked for LocalTalk's RTS/CTS exchange: LLAP
// requires a directed transmission to open with lapRTS and send the data
// only after the receiver grants a lapCTS.  The classic 68k .MPP driver
// happens to latch a bare data frame anyway, but Mac OS 8.1's native
// PowerMac LocalTalk driver's receive state machine only accepts a data
// frame it has granted — without the handshake every directed reply
// (e.g. the Chooser's NBP LookupReply) is silently discarded by the
// guest.  Control frames and broadcasts stay direct (broadcasts open
// with an ungated lapRTS to $FF on the wire; our transport delivers
// whole frames, so the preamble is not needed there).
//
// A FIFO, not a single slot: an ATP response arrives here as a burst of up
// to eight directed frames, and every one of them has to complete its own
// RTS/CTS exchange in turn — a single parking slot would drop all but the
// last and stall every multi-packet transfer (the AFP file copy hung on
// exactly that).  One frame is in flight at a time; the next RTS goes out
// when the previous frame's CTS-triggered send has happened.
#define LLAP_RTS_QUEUE_DEPTH 32

typedef struct {
    uint8_t dst;
    size_t len;
    uint8_t buf[3 + LLAP_DATA_MAX_SIZE]; // header + payload
} llap_queued_frame_t;

// The RTS/CTS queue of one connection.
typedef struct {
    llap_queued_frame_t q[LLAP_RTS_QUEUE_DEPTH];
    int head, count; // ring; q[head] is the frame whose RTS is on the wire
    bool rts_out; // an RTS for q[head] has been sent and awaits its CTS
    int attempts; // RTSs sent for q[head] without a CTS
    uint32_t generation; // bumps on every RTS; a stale timeout is ignored
} llap_rts_queue_t;

#define ATP_MAX_HANDLERS     8
#define ATP_MAX_OUTGOING     16
#define ATP_MAX_XO_CACHE     16
#define ATP_DEFAULT_RETRY_MS 2000u

// A server socket's request handler (ASP on 8 and 54, PAP on 6).
typedef struct {
    bool in_use;
    uint8_t socket;
    atp_socket_handler_t handler;
    void *ctx;
} atp_handler_slot_t;

// One transaction this host originated.
struct atp_request_handle {
    bool in_use;
    bool xo;
    bool infinite_retries;
    uint8_t src_socket;
    uint16_t tid;
    uint8_t base_ctl;
    uint8_t trel_hint;
    uint8_t initial_bitmap;
    uint8_t pending_bitmap;
    uint8_t user[4];
    uint8_t payload[ATP_MAX_ATP_PAYLOAD];
    int payload_len;
    atalk_socket_addr_t dest;
    uint64_t retry_timeout_ns;
    int retries_remaining;
    uint32_t timer_generation;
    atp_request_callbacks_t callbacks;
    void *cb_ctx;
};

typedef struct {
    bool valid;
    uint8_t seq;
    int len;
    uint8_t bytes[DDP_MAX_DATA_SIZE];
} atp_resp_packet_cache_t;

// An exactly-once transaction the guest sent us: the response, kept for a
// retransmitted request until the release timer or the guest's TRel.
typedef struct {
    bool in_use;
    bool response_ready;
    uint16_t tid;
    uint8_t requester_node;
    uint8_t requester_socket;
    uint8_t responder_socket;
    uint8_t trel_hint;
    uint32_t release_generation;
    atp_resp_packet_cache_t packets[ATP_MAX_RESPONSE_FRAGMENTS];
} atp_xo_entry_t;

#define NBP_OBJECT_MAX            ATALK_NBP_NAME_MAX
#define NBP_TYPE_MAX              ATALK_NBP_NAME_MAX
#define NBP_ZONE_MAX              ATALK_NBP_NAME_MAX
#define NBP_MAX_ENTRIES           ATALK_NBP_MAX_ENTRIES
#define NBP_MAX_TUPLES_PER_PACKET 8
#define NBP_APPROX_CHAR           0xC5 // MacRoman "≈" wildcard per Inside AppleTalk

struct atalk_nbp_entry {
    bool in_use;
    char object[NBP_OBJECT_MAX + 1];
    char type[NBP_TYPE_MAX + 1];
    char zone[NBP_ZONE_MAX + 1];
    uint8_t object_len;
    uint8_t type_len;
    uint8_t zone_len;
    uint16_t net;
    uint8_t node;
    uint8_t socket;
    uint8_t enumerator;
};

// One outstanding outgoing lookup.  The PPC browse is the only client, and it
// re-issues rather than queueing, so a single slot is enough.
typedef struct {
    bool active;
    uint8_t nbp_id;
    atalk_nbp_reply_fn cb;
    void *ctx;
} nbp_lookup_t;

// Every timer one connection has registered with its machine's scheduler,
// so unplugging it can drop their pending events: the link and transport
// timers below, and those of the layers above (ASP, ADSP, the printer's PAP
// and LaserWriter timers).
#define ATALK_MAX_TIMERS 16

// ============================================================================
// The network and the connection (appletalk.h, "Lifecycle")
// ============================================================================

// A machine's connection to the network: its end of the cable and everything
// that exists only between the network and that one Mac -- the link's state
// and counters, the RTS/CTS exchange and the wire's timing, ATP transactions
// in either direction, the NBP lookup in flight, and the session tables of
// the layers above.
struct atalk_conn {
    scc_t *scc;
    scheduler_t *scheduler;
    // Attached to the link.  The object model is the user's switch to take
    // the machine off the network; every frame in and out passes this guard.
    bool enabled;
    atalk_stats_t stats; // published as `appletalk.stats`

    atalk_timer_t *timers[ATALK_MAX_TIMERS];
    int num_timers;

    // LLAP
    llap_rts_queue_t llap_rts;
    atalk_timer_t llap_rts_timer; // CTS did not come: retry the RTS (data = generation)
    atalk_timer_t llap_kick_timer; // the wire is free again: open the next dialog
    double wire_busy_until_ns; // scheduler time the wire frees up
    // Set between answering the guest's lapRTS with lapCTS and the arrival of
    // its data frame: the wire is reserved for that dialog, and our own lapRTS
    // must wait (see llap_wire_note_peer_frame).
    bool peer_reserved;

    // ATP
    atp_request_handle_t atp_requests[ATP_MAX_OUTGOING];
    atp_xo_entry_t xo_entries[ATP_MAX_XO_CACHE];
    uint32_t next_tid; // atalk_id_alloc cursor
    // Per-request retry and per-transaction XO release; many can be pending at
    // once, told apart by their data (atp_encode_event_data).
    atalk_timer_t atp_retry_timer;
    atalk_timer_t atp_release_timer;

    // NBP
    nbp_lookup_t nbp_lookup;
    uint8_t nbp_next_lookup_id;

    // The layers above: their sessions with this Mac
    asp_link_t *asp;
    afp_link_t *afp;
    adsp_link_t *adsp;
    ppc_link_t *ppc;
    aevt_link_t *aevt;
    pap_link_t *pap;
};

// The network: one per process, created by appletalk_network_init.  The
// nodes it provides keep their own state in their modules (the AFP server in
// afp_volume.c, the printer in appletalk_printer.c and laserwriter_job.c, the
// program-linking peer in appletalk_ppc.c and appletalk_aevt.c); what is
// gathered here is the registry they share and the cable.
struct atalk_network {
    bool up; // appletalk_network_init has run
    // The cable: the one connection the network is serving, or NULL.  The
    // active machine's connection is plugged in (atalk_conn_plug, from
    // system_swap_in); deleting it leaves the cable empty.
    atalk_conn_t *plugged;
    atalk_nbp_entry_t nbp_entries[NBP_MAX_ENTRIES]; // names this host registers
    // Enumerator cursor.  One for every socket: an enumerator only has to
    // be unique on its own socket, which nbp_alloc_enumerator checks, so a
    // cursor per socket number (256 of them) bought nothing.
    uint8_t nbp_next_enum;
    atp_handler_slot_t atp_handlers[ATP_MAX_HANDLERS];
    // The nodes' parts, each made by its module when the network comes up
    // and reached by that module through its own pointer to it: the file
    // server and the ASP sessions it serves, the LaserWriter, and the
    // program-linking peer (ADSP carries PPC sessions, which carry Apple
    // events).  They live as long as the network, which is the process.
    asp_server_t *asp;
    afp_server_t *afp;
    pap_printer_t *printer;
    adsp_host_t *adsp;
    ppc_host_t *ppc;
    aevt_host_t *aevt;
};

static atalk_network_t g_net;

// The connection a timer belongs to, from the address of its timer member.
#define CONN_OF(source, member) ((atalk_conn_t *)(void *)((char *)(source) - offsetof(atalk_conn_t, member)))

// Counters read while no machine is plugged in.
static const atalk_stats_t k_no_stats;

scheduler_t *atalk_scheduler(void) {
    return g_net.plugged ? g_net.plugged->scheduler : NULL;
}

uint64_t atalk_now_ns(void) {
    scheduler_t *s = atalk_scheduler();
    return s ? (uint64_t)scheduler_time_ns(s) : 0;
}

// === Timers (appletalk_internal.h) ===========================================

void atalk_timer_init(atalk_conn_t *c, atalk_timer_t *t, const char *source_name, const char *event_name,
                      atalk_timer_fn cb) {
    GS_ASSERT(c && t && cb && c->scheduler);
    if (!c || !t || !cb || !c->scheduler)
        return;
    t->cb = cb;
    scheduler_new_event_type(c->scheduler, source_name, t, event_name, cb);
    for (int i = 0; i < c->num_timers; i++)
        if (c->timers[i] == t)
            return;
    GS_ASSERT(c->num_timers < ATALK_MAX_TIMERS);
    if (c->num_timers < ATALK_MAX_TIMERS)
        c->timers[c->num_timers++] = t;
}

void atalk_timer_arm(atalk_timer_t *t, uint64_t data, uint64_t delay_ns) {
    scheduler_t *s = atalk_scheduler();
    if (!s)
        return;
    GS_ASSERT(t->cb); // a registration hook missed atalk_timer_init
    if (delay_ns < ATALK_TIMER_MIN_NS)
        delay_ns = ATALK_TIMER_MIN_NS;
    remove_event_by_data(s, t->cb, t, data);
    scheduler_new_cpu_event(s, t->cb, t, data, 0, delay_ns);
}

void atalk_timer_cancel(atalk_timer_t *t, uint64_t data) {
    scheduler_t *s = atalk_scheduler();
    if (s && t->cb)
        remove_event_by_data(s, t->cb, t, data);
}

void atalk_timer_cancel_all(atalk_timer_t *t) {
    scheduler_t *s = atalk_scheduler();
    if (s && t->cb)
        remove_event(s, t->cb, t);
}

// Drop every pending event of the timers the connection registered: it is
// coming off the cable.  The registrations stay with its scheduler, which
// goes with its machine.
static void atalk_timers_forget(atalk_conn_t *c) {
    if (!c->scheduler)
        return;
    for (int i = 0; i < c->num_timers; i++)
        scheduler_forget_source(c->scheduler, c->timers[i]);
}

// The connection's checkpoint block -- all a machine checkpoint carries of
// AppleTalk.  The network is not in it: the shares, the server's identity,
// the printer and the program-linking peer are host state.  Nor are the
// sessions: a restored connection has none, which is what the guest sees
// when a server restarts.  The session numbering keeps the restored server
// from handing out a session reference or wire id the guest still holds, so
// a request on a stale session is refused rather than taken for a new one's.
// The ATP TID cursor is kept for the same reason: a restored connection that
// restarted at 0x2000 would reuse TIDs the guest's responder may still hold
// in its exactly-once cache, and take a new request for a retransmission --
// and a replay from the checkpoint numbers its requests as the original did.
#define ATALK_PERSIST_MAGIC 0x41544B32u // 'ATK2'

typedef struct {
    uint32_t magic;
    bool enabled;
    uint8_t next_sess_id;
    uint16_t next_sess_ref;
    uint32_t next_tid; // ATP TID cursor (atalk_conn.next_tid)
    uint32_t reserved; // zero; keeps stats 8-byte aligned
    atalk_stats_t stats;
} atalk_persist_t;

// ============================================================================
// Forward Declarations
// ============================================================================

// Lower layers at top of file, higher at bottom. These prototypes resolve circular references.
static void ddp_short_in(atalk_conn_t *c, llap_header_t *llap, const uint8_t *buf, size_t len);
static void ddp_in(atalk_conn_t *c, ddp_header_t *ddp, const uint8_t *buf, size_t len);
static void nbp_in(atalk_conn_t *c, ddp_header_t *ddp, const uint8_t *buf, size_t len);
static void atp_in(atalk_conn_t *c, const ddp_header_t *ddp, const uint8_t *buf, int len);
static void atp_timers_init(atalk_conn_t *c);
static void atp_reset(atalk_conn_t *c, bool teardown);
// Logging category function used by LOG() macro; provided by LOG_USE_CATEGORY_NAME later
static log_category_t *log_local_category(void);
static void log_hex(log_category_t *cat, int level, const char *tag, const uint8_t *data, size_t len);
static log_category_t *llap_log_category(void);
static log_category_t *atp_log_category(void);
#define LOG_LLAP(level, fmt, ...) LOG_WITH(llap_log_category(), (level), (fmt), ##__VA_ARGS__)
#define LOG_ATP(level, fmt, ...)  LOG_WITH(atp_log_category(), (level), (fmt), ##__VA_ARGS__)

// Every frame the stack discards goes through here, counted by why, so
// `appletalk.stats` says what was thrown away.  There is no
// CRC anywhere in the stack -- the SCC hands over whole frames -- so there is
// no "CRC error"; what the old crc_errors counted was malformed frames, and
// only three of the thirty-odd places that drop one counted anything.
typedef enum {
    ATALK_DROP_MALFORMED, // cannot be parsed: short, lengths disagree, bad type
    ATALK_DROP_UNHANDLED, // well-formed, but nothing here serves it
    ATALK_DROP_TX, // one of ours that could not be transmitted
} atalk_drop_t;

static void atalk_drop(atalk_conn_t *c, atalk_drop_t kind, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
    ;

// ============================================================================
// Operations
// ============================================================================

// =============================== LLAP (LocalTalk) - lowest layer ===============================

// A node that does not answer lapRTS with lapCTS is busy or gone.  Real
// LLAP waits an interframe gap (200 us) for the CTS, retries the RTS up to
// 32 times and then gives up on that frame — it does NOT hold every later
// frame hostage.  Without the timeout a single unanswered RTS (the guest's
// .MPP mid-hunt, or simply not listening yet) wedged the queue for good and
// every subsequent directed frame — NBP replies included — was lost.
// The wait is deliberately far longer than the real 200 us: an emulated
// Mac Plus answers in ~500 us of guest time, and a retry that lands before
// its CTS opens a duplicate exchange (two CTSs, one data frame) that leaves
// the driver waiting for data that never comes.  The timeout only has to
// unwedge a dead exchange, so a lazy 2 ms x 8 does that without ever racing
// a live one.
#define LLAP_RTS_TIMEOUT_NS   2000000.0
#define LLAP_RTS_MAX_ATTEMPTS 8

// LocalTalk is 230.4 kbit/s: ~35 us per byte on the wire.  Our transport
// hands a whole frame to the guest's SCC at once, but the guest's driver
// still spends the frame's real wire time taking it (the PDM's .MPP reads
// the rest of a frame through the AMIC DMA engine, which paces itself at
// that rate).  A lapRTS for the NEXT frame must not go out until the wire
// would be free — on real LocalTalk it physically cannot — or it arrives
// while the driver is inside ReadRest, is never answered, and the RTS
// timeout below throws the frame away (the AFP copy lost every frame but
// the first of each 8-frame ATP response that way).
#define LLAP_BYTE_NS 35000.0
#define LLAP_IFG_NS  200000.0 // interframe gap before the next dialog opens

// The longest LLAP frame on the wire (header + data + CRC + flags), the
// reservation's ceiling should the guest never send the frame it asked for.
#define LLAP_MAX_FRAME_NS ((3.0 + LLAP_DATA_MAX_SIZE + 4.0) * LLAP_BYTE_NS)
static void llap_rts_timeout_cb(void *source, uint64_t data);
static void llap_rts_kick_cb(void *source, uint64_t data);
static void llap_wire_send(atalk_conn_t *c, const uint8_t *buf, size_t total);

static void llap_timers_init(atalk_conn_t *c) {
    atalk_timer_init(c, &c->llap_rts_timer, "llap", "rts_timeout", &llap_rts_timeout_cb);
    atalk_timer_init(c, &c->llap_kick_timer, "llap", "rts_kick", &llap_rts_kick_cb);
}

static void llap_rts_reset(atalk_conn_t *c) {
    atalk_timer_cancel_all(&c->llap_rts_timer);
    atalk_timer_cancel_all(&c->llap_kick_timer);
    memset(&c->llap_rts, 0, sizeof(c->llap_rts));
    c->wire_busy_until_ns = 0;
    c->peer_reserved = false;
}

// A data frame of `total` bytes just went out: the wire stays busy for its
// transmission time plus the interframe gap.
static void llap_wire_note_busy(atalk_conn_t *c, size_t total) {
    if (!c->scheduler)
        return;
    double until = scheduler_time_ns(c->scheduler) + (double)total * LLAP_BYTE_NS + LLAP_IFG_NS;
    if (until > c->wire_busy_until_ns)
        c->wire_busy_until_ns = until;
}

// The guest's frames occupy the wire too.  The SCC completes the driver's
// transmission the instant it finishes writing (wr0's faked underrun), so a
// frame reaches us with none of its wire time elapsed: hold the wire for that
// time plus the interframe gap, as llap_wire_note_busy does for our own.
// Without this our lapRTS went out in the same instant the guest finished a
// frame -- or between its lapRTS and the data frame that follows -- which on
// real LocalTalk cannot happen; the driver, still transmitting, never saw it,
// and a read it had outstanding never completed.
static void llap_wire_note_peer_frame(atalk_conn_t *c, size_t total) {
    if (!c->scheduler)
        return;
    double until = scheduler_time_ns(c->scheduler) + (double)total * LLAP_BYTE_NS + LLAP_IFG_NS;
    if (c->peer_reserved) {
        // The frame the reservation was for has arrived; only its own wire
        // time remains, not the reservation's ceiling.
        c->peer_reserved = false;
        c->wire_busy_until_ns = until;
    } else if (until > c->wire_busy_until_ns) {
        c->wire_busy_until_ns = until;
    }
}

// We answered the guest's lapRTS with lapCTS: its data frame is next on the
// wire.  Reserve the wire until it arrives, up to the longest frame.
static void llap_wire_reserve_for_peer(atalk_conn_t *c) {
    if (!c->scheduler)
        return;
    double until = scheduler_time_ns(c->scheduler) + LLAP_IFG_NS + LLAP_MAX_FRAME_NS;
    if (until > c->wire_busy_until_ns)
        c->wire_busy_until_ns = until;
    c->peer_reserved = true;
}

static void llap_rts_kick(atalk_conn_t *c);

static void llap_rts_kick_cb(void *source, uint64_t data) {
    (void)data;
    llap_rts_kick(CONN_OF(source, llap_kick_timer));
}

// Put the RTS for the frame at the queue head on the wire (if any) — once
// the wire is free.
static void llap_rts_kick(atalk_conn_t *c) {
    if (c->llap_rts.rts_out || c->llap_rts.count == 0)
        return;
    if (c->scheduler) {
        double now = scheduler_time_ns(c->scheduler);
        if (now < c->wire_busy_until_ns) {
            // atalk_timer_arm never waits less than ATALK_TIMER_MIN_NS: a
            // sub-ns remainder would fire with time unchanged and re-arm
            // forever.
            atalk_timer_arm(&c->llap_kick_timer, 0, (uint64_t)(c->wire_busy_until_ns - now));
            return;
        }
    }
    llap_queued_frame_t *f = &c->llap_rts.q[c->llap_rts.head];
    uint8_t rts[3] = {f->dst, f->buf[1], LLAP_RTS};
    c->llap_rts.rts_out = true;
    c->llap_rts.attempts++;
    c->llap_rts.generation++;
    LOG_LLAP(8, "LLAP tx: RTS to %02X, %zu-byte data queued for CTS (%d queued, attempt %d)", f->dst, f->len,
             c->llap_rts.count, c->llap_rts.attempts);
    llap_wire_send(c, rts, sizeof(rts));
    atalk_timer_arm(&c->llap_rts_timer, c->llap_rts.generation, (uint64_t)LLAP_RTS_TIMEOUT_NS);
}

static void llap_rts_timeout_cb(void *source, uint64_t data) {
    atalk_conn_t *c = CONN_OF(source, llap_rts_timer);
    if (!c->llap_rts.rts_out || data != c->llap_rts.generation)
        return; // the CTS came (or a newer RTS is out): stale timeout
    c->llap_rts.rts_out = false;
    if (c->llap_rts.attempts >= LLAP_RTS_MAX_ATTEMPTS) {
        llap_queued_frame_t *f = &c->llap_rts.q[c->llap_rts.head];
        LOG_LLAP(2, "LLAP tx: no CTS from %02X after %d RTS attempts, dropping a %zu-byte frame", f->dst,
                 c->llap_rts.attempts, f->len);
        c->stats.tx_dropped++;
        c->llap_rts.head = (c->llap_rts.head + 1) % LLAP_RTS_QUEUE_DEPTH;
        c->llap_rts.count--;
        c->llap_rts.attempts = 0;
    }
    llap_rts_kick(c); // retry this frame's RTS, or open the next frame's exchange
}

static void llap_wire_send(atalk_conn_t *c, const uint8_t *buf, size_t total) {
    // llap_send checks this too, but a frame queued for its CTS goes out from
    // the RTS timer, which does not pass through llap_send again.
    if (!c->enabled)
        return;
    log_hex(llap_log_category(), 11, "LLAP tx dump", buf, total);
    c->stats.llap_tx++;
    if (c->scc)
        scc_sdlc_send(c->scc, (uint8_t *)buf, total);
}

// Returns 0 on success, -1 if `len` exceeds the LLAP payload max (caller bug).
// We refuse to transmit truncated frames rather than silently emit a corrupted
// one — the peer would see a malformed LLAP and discard it anyway, but in
// our local trace it would look like a successful send.
static int llap_send(atalk_conn_t *c, const llap_header_t *llap, const uint8_t *data, size_t len) {
    if (!c->enabled)
        return -1; // the stack is detached from the link
    if (c->scc && !scc_sdlc_ready(c->scc)) {
        // Nothing is listening yet: the guest has not put the SCC into SDLC
        // mode, so its AppleTalk driver is not loaded.  Replies never reach
        // here (they answer a frame the guest just sent), but traffic we
        // originate can, and it must not be forced onto a dead link.
        LOG_LLAP(4, "LLAP tx: dropped, the guest's AppleTalk driver is not up");
        c->stats.tx_dropped++;
        return -1;
    }
    if (len > LLAP_DATA_MAX_SIZE) {
        LOG_LLAP(1, "LLAP tx: refused oversize frame (%zu > %d)", len, LLAP_DATA_MAX_SIZE);
        c->stats.tx_dropped++;
        return -1;
    }
    uint8_t buf[LLAP_HEADER_SIZE + LLAP_DATA_MAX_SIZE];

    buf[0] = llap->dst;
    buf[1] = llap->src;
    buf[2] = llap->type;

    if (len > 0 && data) {
        memcpy((char *)(buf + 3), data, len);
    }

    size_t total = len + LLAP_HEADER_SIZE;

    // Directed DATA frames go through the RTS/CTS exchange (see c->llap_rts).
    if (llap->dst != 0xFF && llap->type < 0x80) {
        if (c->llap_rts.count >= LLAP_RTS_QUEUE_DEPTH) {
            // The wire cannot keep up; the upper layers (ATP) retransmit.
            LOG_LLAP(2, "LLAP tx: RTS queue full, dropping a %zu-byte frame to %02X", total, llap->dst);
            c->stats.tx_dropped++;
            return -1;
        }
        int slot = (c->llap_rts.head + c->llap_rts.count) % LLAP_RTS_QUEUE_DEPTH;
        llap_queued_frame_t *f = &c->llap_rts.q[slot];
        memcpy(f->buf, buf, total);
        f->len = total;
        f->dst = llap->dst;
        c->llap_rts.count++;
        llap_rts_kick(c);
        return 0;
    }

    llap_wire_send(c, buf, total);
    return 0;
}

static void atalk_drop(atalk_conn_t *c, atalk_drop_t kind, const char *fmt, ...) {
    static const char *const names[] = {"malformed", "unhandled", "not transmitted"};
    switch (kind) {
    case ATALK_DROP_MALFORMED:
        c->stats.malformed++;
        break;
    case ATALK_DROP_UNHANDLED:
        c->stats.unhandled++;
        break;
    case ATALK_DROP_TX:
        c->stats.tx_dropped++;
        break;
    }
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    LOG(5, "dropped (%s): %s", names[kind], msg);
}

static void llap_in(atalk_conn_t *c, const uint8_t *buf, size_t len) {
    llap_header_t header;

    if (!c->enabled)
        return; // the stack is detached from the link

    // Short/malformed packets can arrive from the SCC during A/UX
    // initialization.
    if (len < LLAP_HEADER_SIZE) {
        atalk_drop(c, ATALK_DROP_MALFORMED, "LLAP frame of %zu bytes", len);
        return;
    }
    c->stats.llap_rx++;

    header.dst = buf[0];
    header.src = buf[1];
    header.type = buf[2];
    llap_wire_note_peer_frame(c, len);

    // LLAP rx hexdump at high verbosity
    log_hex(llap_log_category(), 11, "LLAP rx dump", buf, len);

    switch (header.type) {

    case LLAP_ENQ:
        if (len != LLAP_HEADER_SIZE) {
            // Control frames are exactly 3 bytes; drop wire junk instead of
            // dying on it (guest drivers do emit malformed traffic).
            atalk_drop(c, ATALK_DROP_MALFORMED, "LLAP ENQ of %zu bytes", len);
            break;
        }
        LOG_LLAP(11, "LLAP ENQ src=%02X dst=%02X", (unsigned)header.src, (unsigned)header.dst);
        // Reply with ACK if this ENQ targets our node (dynamic node ID probe/ack).
        if (header.dst == ATALK_HOST_NODE) {
            llap_header_t ack;
            ack.dst = header.src;
            ack.src = ATALK_HOST_NODE;
            ack.type = LLAP_ACK;
            LOG_LLAP(11, "LLAP send ACK to=%02X", (unsigned)ack.dst);
            llap_send(c, &ack, NULL, 0);
        }
        break;

    case LLAP_RTS:
        if (len != LLAP_HEADER_SIZE) {
            atalk_drop(c, ATALK_DROP_MALFORMED, "LLAP RTS of %zu bytes", len);
            break;
        }
        LOG_LLAP(11, "LLAP RTS src=%02X dst=%02X", (unsigned)header.src, (unsigned)header.dst);
        // Respond with CTS for directed traffic addressed to us so the sender may transmit.
        if (header.dst == ATALK_HOST_NODE) {
            llap_header_t cts;
            cts.dst = header.src;
            cts.src = ATALK_HOST_NODE;
            cts.type = LLAP_CTS;
            LOG_LLAP(11, "LLAP send CTS to=%02X", (unsigned)cts.dst);
            llap_send(c, &cts, NULL, 0);
            llap_wire_reserve_for_peer(c);
        }
        break;

    case LLAP_CTS:
        if (len != LLAP_HEADER_SIZE) {
            atalk_drop(c, ATALK_DROP_MALFORMED, "LLAP CTS of %zu bytes", len);
            break;
        }
        // The receiver granted our lapRTS: transmit the parked data frame.
        LOG_LLAP(8, "LLAP CTS src=%02X dst=%02X", (unsigned)header.src, (unsigned)header.dst);
        if (c->llap_rts.rts_out && c->llap_rts.count > 0 && header.dst == ATALK_HOST_NODE &&
            header.src == c->llap_rts.q[c->llap_rts.head].dst) {
            llap_queued_frame_t *f = &c->llap_rts.q[c->llap_rts.head];
            c->llap_rts.rts_out = false;
            c->llap_rts.attempts = 0;
            c->llap_rts.head = (c->llap_rts.head + 1) % LLAP_RTS_QUEUE_DEPTH;
            c->llap_rts.count--;
            llap_wire_send(c, f->buf, f->len);
            llap_wire_note_busy(c, f->len);
            llap_rts_kick(c); // next queued frame opens its own exchange (after the wire frees up)
        }
        break;

    case LLAP_DDP_SHORT:
        LOG_LLAP(11, "LLAP DDP_SHORT rx len=%zu", len - 3);
        ddp_short_in(c, &header, buf + 3, len - 3);
        break;

    case LLAP_DDP_EXTENDED:
        // LocalTalk nodes use short headers; extended DDP is for routers.
        atalk_drop(c, ATALK_DROP_UNHANDLED, "extended DDP from %02X", (unsigned)header.src);
        break;

    default:
        atalk_drop(c, ATALK_DROP_UNHANDLED, "LLAP type %02X from %02X", (unsigned)header.type, (unsigned)header.src);
        break;
    }
}

// The SCC's frame sink (scc_set_frame_sink): a frame the guest transmitted on
// the LocalTalk port of the machine whose connection `ctx` is.  The sink is
// installed only while that connection is plugged in.
// Every frame, and so every protocol layer above, enters here.  The stack's
// state -- the network, the connection, the server tables -- is unlocked
// globals, sound because all of it runs on the one worker thread: the
// guest's frames and the scheduler's timers there, the object model's calls
// through the mailbox.  A debug build checks it.
static void llap_receive(void *ctx, const uint8_t *buf, size_t size) {
    worker_thread_check("llap_receive");
    llap_in((atalk_conn_t *)ctx, buf, size);
}

// =============================== DDP (Datagram Delivery Protocol) ===============================

// [1]: ddp type field values (shared with the ADSP/PPC modules)
#define DDP_RTMP_RESPONSE DDP_TYPE_RTMP_RESPONSE
#define DDP_NBP           DDP_TYPE_NBP
#define DDP_ATP           DDP_TYPE_ATP
#define DDP_AEP           DDP_TYPE_AEP
#define AEP_ECHO_REQUEST  1
#define AEP_ECHO_REPLY    2
#define DDP_RTMP_REQUEST  DDP_TYPE_RTMP_REQUEST
#define DDP_ZIP           DDP_TYPE_ZIP
#define DDP_ADSP          DDP_TYPE_ADSP

// Returns a short mnemonic name for a DDP protocol type or NULL if unknown.
static const char *ddp_type_name(uint8_t type) {
    switch (type) {
    case DDP_RTMP_RESPONSE:
        return "RTMP-Resp";
    case DDP_NBP:
        return "NBP";
    case DDP_ATP:
        return "ATP";
    case DDP_AEP:
        return "AEP";
    case DDP_RTMP_REQUEST:
        return "RTMP-Req";
    case DDP_ZIP:
        return "ZIP";
    case DDP_ADSP:
        return "ADSP";
    default:
        return NULL;
    }
}

static void ddp_log_summary(int level, const char *direction, const ddp_header_t *ddp, uint16_t total_len,
                            size_t payload_len) {
    if (!ddp || !direction)
        return;
    const char *type_name = ddp_type_name(ddp->type);
    if (type_name) {
        LOG(level, "DDP %s [%s] type=0x%02X total=%u payload=%zu dstSock=%u srcSock=%u", direction, type_name,
            (unsigned)ddp->type, (unsigned)total_len, payload_len, (unsigned)ddp->dst_socket,
            (unsigned)ddp->src_socket);
    } else {
        LOG(level, "DDP %s type=0x%02X total=%u payload=%zu dstSock=%u srcSock=%u", direction, (unsigned)ddp->type,
            (unsigned)total_len, payload_len, (unsigned)ddp->dst_socket, (unsigned)ddp->src_socket);
    }
}

// Construct a DDP reply header by reversing source and destination
static void ddp_setup_reply(const ddp_header_t *request, ddp_header_t *reply) {
    reply->llap.dst = request->llap.src;
    reply->llap.src = ATALK_HOST_NODE;
    reply->llap.type = request->llap.type;

    reply->len = request->len;
    reply->dst_net = request->src_net;
    reply->src_net = request->dst_net;

    reply->dst_socket = request->src_socket;
    reply->src_socket = request->dst_socket;
    reply->type = request->type; // preserve DDP protocol type in reply
}

// ============================================================================
// Lifecycle: the network
// ============================================================================

atalk_network_t *appletalk_network_init(void) {
    if (g_net.up)
        return &g_net;
    // The nodes on the cable, each publishing its NBP name and taking its
    // sockets: the file server over ASP (8 and 54), the LaserWriter (6), and
    // the program-linking peer -- ADSP carries PPC sessions, which carry Apple
    // events (docs/internals/core/network/ppc_appleevents.md §1).
    // Each in turn: the file server is ASP's client, and the Apple-event
    // layer publishes PPC's host port.
    if (!(g_net.asp = asp_init()) || !(g_net.afp = atalk_server_init()) ||
        !(g_net.printer = atalk_printer_register()) || !(g_net.adsp = atalk_adsp_init()) ||
        !(g_net.ppc = atalk_ppc_init()) || !(g_net.aevt = atalk_aevt_init())) {
        LOG(0, "Error: out of memory creating the AppleTalk network");
        return NULL;
    }
    // The ImageWriter's LocalTalk Option card: published only while the
    // machine's printer is on LocalTalk
    atalk_imagewriter_register();
    g_net.up = true;
    atalk_install_objects();
    return &g_net;
}

atalk_network_t *appletalk_network(void) {
    return g_net.up ? &g_net : NULL;
}

// ============================================================================
// Lifecycle: the connection
// ============================================================================

// Put `c` on the cable: the network serves its machine from now on.
static void atalk_conn_plug_in(atalk_conn_t *c) {
    g_net.plugged = c;
    if (c->scc)
        scc_set_frame_sink(c->scc, llap_receive, c);
    asp_plug(c->asp);
    afp_plug(c->afp);
    atalk_adsp_plug(c->adsp);
    atalk_ppc_plug(c->ppc);
    atalk_aevt_plug(c->aevt);
    atalk_printer_plug(c->pap);
}

// Register every timer the connection and the layers above use with its
// machine's scheduler, while the machine is being built: before a checkpoint
// restore replays the saved queue, and before the machine is plugged in
// (atalk_timer_t).
static void atalk_conn_register_timers(atalk_conn_t *c) {
    if (!c->scheduler)
        return;
    llap_timers_init(c);
    atp_timers_init(c);
    asp_link_register_timers(c, c->asp);
    atalk_adsp_link_register_timers(c, c->adsp);
    atalk_printer_register_timers(c, c->pap);
    atalk_imagewriter_register_timers(c);
}

// Take `c` off the cable, as a server sees a Mac vanish: every session with
// it closes and every transaction with it ends, and the network itself --
// shares, names, printer -- is untouched.
static void atalk_conn_unplug(atalk_conn_t *c) {
    // Sessions first, top down: closing them hands the AFP layer its forks
    // back, and ADSP's close puts CLOSE advice on the wire.
    atalk_asp_close_all_sessions();
    atalk_aevt_plug(NULL);
    atalk_ppc_plug(NULL);
    atalk_adsp_plug(NULL);
    atalk_printer_plug(NULL);
    atalk_imagewriter_unplug();
    afp_plug(NULL);
    asp_plug(NULL);

    // Then the transport: nothing above can call into it any more, so
    // requests are dropped without completing.
    atp_reset(c, true);
    atalk_nbp_lookup_cancel();
    llap_rts_reset(c);

    // Every timer last: the closes above can still transmit, and a frame that
    // waits for a CTS arms an LLAP timer.
    atalk_timers_forget(c);

    if (c->scc)
        scc_set_frame_sink(c->scc, NULL, NULL);
    g_net.plugged = NULL;
}

atalk_conn_t *atalk_conn_new(atalk_network_t *network, scheduler_t *scheduler, scc_t *scc, checkpoint_t *checkpoint) {
    (void)network; // unused when asserts compile out
    GS_ASSERT(network == &g_net && network->up);
    atalk_conn_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->scc = scc;
    c->scheduler = scheduler;
    c->enabled = true;
    c->next_tid = 0x2000;
    c->nbp_next_lookup_id = 1;
    c->asp = asp_link_new();
    c->afp = afp_link_new();
    c->adsp = atalk_adsp_link_new();
    c->ppc = atalk_ppc_link_new();
    c->aevt = atalk_aevt_link_new();
    c->pap = atalk_printer_link_new();
    if (!c->asp || !c->afp || !c->adsp || !c->ppc || !c->aevt || !c->pap) {
        atalk_conn_delete(c);
        return NULL;
    }

    // The connection's block.  The reader zero-fills on failure, and a
    // checkpoint in error is not applied.
    if (checkpoint) {
        atalk_persist_t saved;
        system_read_checkpoint_data(checkpoint, &saved, sizeof(saved), "appletalk");
        if (!checkpoint_has_error(checkpoint) && saved.magic == ATALK_PERSIST_MAGIC) {
            // A bool read off disk may hold any byte; take it as a byte.
            uint8_t enabled_byte;
            memcpy(&enabled_byte, &saved.enabled, 1);
            c->enabled = enabled_byte != 0;
            c->stats = saved.stats;
            c->next_tid = saved.next_tid & 0xFFFFu;
            asp_link_set_numbering(c->asp, saved.next_sess_ref, saved.next_sess_id);
            LOG(1, "atalk: connection restored from checkpoint (%s)", c->enabled ? "enabled" : "disabled");
        }
    }

    // Built, not plugged in: the machine takes the cable when it becomes the
    // active one (atalk_conn_plug), so a build that fails touches nothing.
    atalk_conn_register_timers(c);
    return c;
}

void atalk_conn_plug(atalk_conn_t *c) {
    if (!c || g_net.plugged == c)
        return;
    if (g_net.plugged)
        atalk_conn_unplug(g_net.plugged);
    atalk_conn_plug_in(c);
}

void atalk_conn_checkpoint(const atalk_conn_t *c, checkpoint_t *checkpoint) {
    if (!c || !checkpoint)
        return;
    atalk_persist_t out;
    memset(&out, 0, sizeof(out));
    out.magic = ATALK_PERSIST_MAGIC;
    out.enabled = c->enabled;
    out.next_tid = c->next_tid;
    asp_link_numbering(c->asp, &out.next_sess_ref, &out.next_sess_id);
    out.stats = c->stats;
    system_write_checkpoint_data(checkpoint, &out, sizeof(out), "appletalk");
}

// Deleting a connection unplugs it only if it is the one on the cable: the
// machine a new one replaced was unplugged when that one took the cable, and
// its delete leaves the new machine's connection alone; a build that failed
// was never plugged in.
void atalk_conn_delete(atalk_conn_t *c) {
    if (!c)
        return;
    if (g_net.plugged == c)
        atalk_conn_unplug(c);
    atalk_printer_link_free(c->pap);
    atalk_aevt_link_free(c->aevt);
    atalk_ppc_link_free(c->ppc);
    atalk_adsp_link_free(c->adsp);
    afp_link_free(c->afp);
    asp_link_free(c->asp);
    free(c);
}

// === Stack-level object-model accessors ====================================

bool atalk_get_enabled(void) {
    return g_net.plugged && g_net.plugged->enabled;
}

int atalk_set_enabled(bool enabled, char *err, size_t err_len) {
    atalk_conn_t *c = g_net.plugged;
    if (!c) {
        snprintf(err, err_len, "no machine is connected to the network");
        return -1;
    }
    if (enabled == c->enabled)
        return 0;
    c->enabled = enabled;
    if (!enabled) {
        // Detaching from the link strands every session; drop them rather than
        // leave forks and locks held by clients that can no longer be reached.
        atalk_asp_close_all_sessions();
        atalk_ppc_close_all("the stack was detached from the link");
        adsp_close_all(atalk_adsp_stack(), "the stack was detached from the link");
        atalk_printer_link_down();
        atalk_imagewriter_unplug();
        // ...and nothing below them keeps talking: outgoing requests end as
        // ABORTED, the lookup is cancelled, and frames waiting for a CTS are
        // dropped.
        atp_reset(c, false);
        atalk_nbp_lookup_cancel();
        llap_rts_reset(c);
    }
    LOG(1, "atalk: stack %s", enabled ? "attached to the link" : "detached from the link");
    return 0;
}

// Our LLAP node address is fixed (ATALK_HOST_NODE) rather than acquired by the
// dynamic-node-assignment probe, so it is known as soon as the stack is up.
unsigned atalk_node_id(void) {
    return atalk_get_enabled() ? ATALK_HOST_NODE : 0;
}

const atalk_stats_t *atalk_get_stats(void) {
    return g_net.plugged ? &g_net.plugged->stats : &k_no_stats;
}

// Send a DDP packet to the Mac via LocalTalk
// Returns 0 once the frame is on the link, -1 if it could not be sent (the
// stack is detached, the guest's driver is not up, or the payload will not
// fit a DDP packet).  Callers that originate traffic report that upwards.
static int ddp_send(atalk_conn_t *c, const ddp_header_t *header, const uint8_t *data, int size) {
    if (!header || size <= 0 || size > DDP_MAX_DATA_SIZE)
        return -1;

    uint8_t buffer[DDP_SHORT_HEADER_SIZE + DDP_MAX_DATA_SIZE];
    uint16_t length = (uint16_t)(size + DDP_SHORT_HEADER_SIZE);

    ddp_log_summary(5, "-> Mac", header, length, (size_t)size);

    // Short DDP header: low 2 bits carry the high bits of the 10-bit length.
    buffer[0] = (uint8_t)((length >> 8) & 0x03);
    buffer[1] = length & 0xFF;
    buffer[2] = header->dst_socket;
    buffer[3] = header->src_socket;
    buffer[4] = header->type;
    memcpy(&buffer[5], data, (size_t)size);

    LOG_INDENT(4);
    log_hex(log_local_category(), 9, "DDP tx dump", buffer, length);
    LOG_INDENT(-4);

    c->stats.ddp_out++;
    return llap_send(c, &header->llap, buffer, length);
}

// Send one datagram to a remote socket.  The higher protocol modules (ADSP,
// and the PPC layer above it) build their own payload and hand it here rather
// than reaching into the DDP header layout themselves.
int atalk_ddp_send_to(const atalk_socket_addr_t *dest, uint8_t src_socket, uint8_t ddp_type, const uint8_t *data,
                      int len) {
    atalk_conn_t *c = g_net.plugged;
    if (!dest || len < 0 || len > DDP_MAX_DATA_SIZE)
        return -1;
    if (!c || !c->enabled)
        return -1; // no machine on the cable, or it is detached from the link
    ddp_header_t ddp;
    memset(&ddp, 0, sizeof(ddp));
    ddp.llap.dst = dest->node;
    ddp.llap.src = ATALK_HOST_NODE;
    ddp.llap.type = LLAP_DDP_SHORT;
    ddp.len = (uint16_t)(len + DDP_SHORT_HEADER_SIZE);
    ddp.dst_net = dest->net;
    ddp.dst_socket = dest->socket;
    ddp.src_socket = src_socket;
    ddp.type = ddp_type;
    return ddp_send(c, &ddp, data, len);
}

// Process an incoming DDP packet and dispatch to appropriate protocol handler
static void ddp_in(atalk_conn_t *c, ddp_header_t *ddp, const uint8_t *buf, size_t len) {
    c->stats.ddp_in++;
    switch (ddp->type) {

    case DDP_NBP:
        // [1] each node implements an nbp process on socket number 2
        if (ddp->dst_socket != 2) {
            LOG(4, "NBP rx on unexpected dstSock=%u (expected 2)", (unsigned)ddp->dst_socket);
        }
        nbp_in(c, ddp, buf, len);
        break;

    case DDP_ATP:
        // Requests go to whoever registered the socket (ASP on 8 and 54, PAP on
        // 6), and responses to whoever sent the request; ATP drops -- and
        // counts -- the rest.  A list of sockets here duplicated that registry
        // and would have dropped the response to any request sent from a
        // socket not on it.
        atp_in(c, ddp, buf, (int)len);
        break;

    case DDP_AEP: {
        // AppleTalk Echo Protocol (Inside AppleTalk ch. 6): the Echoer listens
        // on socket 4, discards a packet with no data, and answers an Echo
        // Request (function 1) by sending it back with the function set to 2,
        // Echo Reply.  It used to echo anything on any socket unchanged -- so a
        // pinging client never saw a reply, only its own request coming back.
        if (ddp->dst_socket != 4) {
            atalk_drop(c, ATALK_DROP_UNHANDLED, "AEP on socket %u", (unsigned)ddp->dst_socket);
            break;
        }
        if (len == 0) {
            atalk_drop(c, ATALK_DROP_MALFORMED, "AEP packet with no data");
            break;
        }
        if (buf[0] != AEP_ECHO_REQUEST) {
            atalk_drop(c, ATALK_DROP_UNHANDLED, "AEP function %u", (unsigned)buf[0]);
            break;
        }
        uint8_t echo[DDP_MAX_DATA_SIZE];
        memcpy(echo, buf, len);
        echo[0] = AEP_ECHO_REPLY;
        ddp_header_t reply;
        ddp_setup_reply(ddp, &reply);
        reply.type = DDP_AEP;
        LOG(3, "AEP: echoing %zu bytes", len);
        ddp_send(c, &reply, echo, (int)len);
        break;
    }

    case DDP_ADSP:
        // Reliable byte streams; the PPC Toolbox endpoint rides on these.
        atalk_adsp_ddp_in(ddp, buf, (int)len);
        break;

    case DDP_RTMP_REQUEST:
        // An RTMP Request (one byte, function 1) asks for a router; there is
        // none on this cable, so it goes unanswered.  Only an odd one is logged.
        if (len == 0) {
            LOG(3, "RTMP req with no payload");
            break;
        }
        if (len != 1 || buf[0] != 1)
            LOG(3, "RTMP req unexpected payload len=%zu first=0x%02X", len, buf[0]);
        break;

    default:
        atalk_drop(c, ATALK_DROP_UNHANDLED, "DDP type %02X", (unsigned)ddp->type);
        break;
    }
}

// Process an incoming short DDP header and dispatch to ddp_in
static void ddp_short_in(atalk_conn_t *c, llap_header_t *llap, const uint8_t *buf, size_t len) {
    ddp_header_t ddp;

    // These three were asserts, on bytes the guest wrote.
    // In a build with asserts one bad frame aborted the emulator; in the
    // browser build, which compiles them out, a frame under five bytes was
    // parsed from stale buffer bytes and passed on with len - 5 wrapped.
    if (len < DDP_SHORT_HEADER_SIZE || len > DDP_MAX_DATA_SIZE + DDP_SHORT_HEADER_SIZE) {
        atalk_drop(c, ATALK_DROP_MALFORMED, "DDP frame of %zu bytes", len);
        return;
    }
    // Decode 10-bit length from short header: low 2 bits from first byte, then full second byte.
    ddp.len = (uint16_t)(((buf[0] & 0x03) << 8) | buf[1]);
    if (ddp.len != len) {
        atalk_drop(c, ATALK_DROP_MALFORMED, "DDP length field %u in a %zu-byte frame", (unsigned)ddp.len, len);
        return;
    }
    // Data for another node, or from us: nobody else is on this wire.
    if (llap->dst != ATALK_HOST_NODE && llap->dst != 0xFF) {
        atalk_drop(c, ATALK_DROP_UNHANDLED, "DDP for node %02X", (unsigned)llap->dst);
        return;
    }

    ddp.llap = *llap;
    ddp.checksum = 0;
    ddp.dst_net = 0;
    ddp.src_net = 0;
    ddp.dst_socket = buf[2];
    ddp.src_socket = buf[3];
    ddp.type = buf[4];

    size_t payload_len = (len > DDP_SHORT_HEADER_SIZE) ? (len - DDP_SHORT_HEADER_SIZE) : 0;
    ddp_log_summary(4, "<- Mac", &ddp, ddp.len, payload_len);

    LOG_INDENT(4);
    // Full DDP hexdump at high verbosity (includes 5-byte header + payload)
    log_hex(log_local_category(), 9, "DDP rx dump", buf, len);
    ddp_in(c, &ddp, buf + 5, len - 5);
    LOG_INDENT(-4);
}

#define NBP_BRRQ       1
#define NBP_LKUP       2
#define NBP_LKUP_REPLY 3
#define NBP_FWDREQ     4

// Returns a terse description of the NBP function code or NULL if unknown.
static const char *nbp_function_name(int function) {
    switch (function) {
    case NBP_BRRQ:
        return "BrRq";
    case NBP_LKUP:
        return "Lookup";
    case NBP_LKUP_REPLY:
        return "LookupReply";
    case NBP_FWDREQ:
        return "FwdReq";
    default:
        return NULL;
    }
}

// The wire packs function and tuple count into one byte, four bits each.
// They were `int : 4` bit-fields here, which gcc and clang make signed: a count
// of 8..15 read back as -8..-1, so an inbound packet with eight or more tuples
// was dropped whole and a reply carrying exactly eight said "8" and carried
// none.  The struct never touches the wire; plain bytes.
typedef struct {
    uint8_t function;
    uint8_t tuple_count;
    uint8_t nbp_id;
} nbp_header_t;

typedef struct {

    uint16_t net;
    uint8_t node;
    uint8_t socket;
    uint8_t enumerator;
    int object_len;
    uint8_t object[33];
    int type_len;
    uint8_t type[33];
    int zone_len;
    uint8_t zone[33];

} nbp_tuple_t;

// Helper: parse an NBP length-prefixed (P-string style) up to 32 bytes.
// Advances *p and reduces *len; writes dst and dst_len on success.
// `dst_cap` is the capacity of `dst`, asserted at compile-time by callers
// who pass `sizeof(field)`; minimum 33 (32 payload + NUL).
static bool nbp_parse_pstr32(const uint8_t **p, int *len, uint8_t *dst, size_t dst_cap, int *dst_len) {
    if (!p || !*p || !len || *len < 1 || !dst || !dst_len || dst_cap < 33)
        return false;
    uint8_t n = (*p)[0];
    if (n > 32)
        return false;
    if (*len < 1 + (int)n)
        return false;
    *dst_len = n;
    if (n)
        memcpy(dst, (*p) + 1, n);
    dst[n] = '\0';
    *p += 1 + n;
    *len -= 1 + n;
    return true;
}

// ATP layer wrappers (parallel to DDP): parse, setup reply, and send

// Logging: implicit category for this file -- DDP, NBP and the stack's own
// business.  LLAP and ATP log under their own categories, the names their
// scheduler sources carry, so one layer can be turned up alone: the whole
// stack logged as "appletalk".
LOG_USE_CATEGORY_NAME("appletalk");

static log_category_t *llap_log_category(void) {
    static log_category_t *cat;
    if (!cat)
        cat = log_register_category("llap");
    return cat;
}
static log_category_t *atp_log_category(void) {
    static log_category_t *cat;
    if (!cat)
        cat = log_register_category("atp");
    return cat;
}

// --------------- Hex dump helper for high-verbosity diagnostics ---------------
// Emit a multi-line hex dump with ASCII gutter to the AppleTalk log category
// One LOG call per 16-byte row: a log record is capped at 512 bytes
// (log_vemit), so a whole frame does not fit in one.  The rows are only
// formatted when the category would print them.
static void log_hex(log_category_t *cat, int level, const char *tag, const uint8_t *data, size_t len) {
    if (!log_would_log(cat, level))
        return;
    if (!data || len == 0) {
        LOG_WITH(cat, level, "%s: <empty>", tag ? tag : "HEX");
        return;
    }
    // Build per-line hex with ASCII gutter (16 bytes per line)
    char hexbuf[3 * 16 + 2 + 1]; // "XX " * 16 + space after 8 + NUL
    char asciibuf[16 + 1]; // 16 chars + NUL

    for (size_t off = 0; off < len; off += 16) {
        size_t n = (len - off) < 16 ? (len - off) : 16;

        // Hex field
        size_t hp = 0;
        for (size_t i = 0; i < 16; i++) {
            size_t left = sizeof(hexbuf) - hp;
            if (i < n) {
                if (left <= 1) {
                    hexbuf[sizeof(hexbuf) - 1] = '\0';
                    break;
                }
                int wrote = snprintf(&hexbuf[hp], left, "%02X ", data[off + i]);
                if (wrote < 0) {
                    hexbuf[hp] = '\0';
                    break;
                }
                size_t adv = (size_t)wrote;
                if (adv >= left) { // truncated; keep NUL and stop
                    hp = sizeof(hexbuf) - 1;
                    hexbuf[hp] = '\0';
                    break;
                }
                hp += adv;
            } else {
                // Pad alignment when fewer than 16 bytes this row
                if (left >= 4) {
                    hexbuf[hp++] = ' ';
                    hexbuf[hp++] = ' ';
                    hexbuf[hp++] = ' ';
                }
            }
            if (i == 7 && hp < sizeof(hexbuf) - 1)
                hexbuf[hp++] = ' ';
        }
        hexbuf[hp] = '\0';

        // ASCII gutter
        for (size_t i = 0; i < n; i++) {
            unsigned char c = data[off + i];
            asciibuf[i] = (c >= 32 && c <= 126) ? (char)c : '.';
        }
        asciibuf[n] = '\0';

        LOG_WITH(cat, level, "%04" PRIx64 ": %s | %s", (uint64_t)off, hexbuf, asciibuf);
    }
}

// =============================== NBP (Name Binding Protocol) ===============================

static void nbp_send(atalk_conn_t *c, const ddp_header_t *ddp_header, const nbp_header_t *nbp_header,
                     const nbp_tuple_t *nbp_tuple);
static void nbp_deliver_lookup_reply(atalk_conn_t *c, uint8_t nbp_id, const nbp_tuple_t *tuples, int count);

// === NBP registry views (object model: `appletalk.nbp`) ====================

int atalk_nbp_entry_max(void) {
    return NBP_MAX_ENTRIES;
}

bool atalk_nbp_entry_in_use(int index) {
    return index >= 0 && index < NBP_MAX_ENTRIES && g_net.nbp_entries[index].in_use;
}

bool atalk_nbp_entry_info(int index, atalk_nbp_info_t *out) {
    if (!atalk_nbp_entry_in_use(index) || !out)
        return false;
    const atalk_nbp_entry_t *e = &g_net.nbp_entries[index];
    memset(out, 0, sizeof(*out));
    macroman_to_utf8((const uint8_t *)e->object, e->object_len, out->object, sizeof(out->object));
    macroman_to_utf8((const uint8_t *)e->type, e->type_len, out->type, sizeof(out->type));
    macroman_to_utf8((const uint8_t *)e->zone, e->zone_len, out->zone, sizeof(out->zone));
    out->socket = e->socket;
    out->node = e->node;
    out->net = e->net;
    return true;
}

static int nbp_entry_index(const atalk_nbp_entry_t *entry) {
    if (!entry)
        return -1;
    for (int i = 0; i < NBP_MAX_ENTRIES; i++) {
        if (&g_net.nbp_entries[i] == entry)
            return i;
    }
    return -1;
}

// The UTF-8 field `src` as MacRoman in `dst` (NUL-terminated, for logging):
// its length, or -1 when MacRoman cannot hold it or it does not fit.
static int nbp_copy_field(char *dst, size_t dst_cap, const char *src, bool allow_empty) {
    if (!dst || dst_cap == 0)
        return -1;
    int len = macroman_from_utf8(src ? src : "", (uint8_t *)dst, dst_cap - 1);
    if (len < 0 || (!allow_empty && len == 0))
        return -1;
    dst[len] = '\0';
    return len;
}

int atalk_nbp_name_check(const char *what, const char *name, char *err, size_t err_len) {
    uint8_t mac[ATALK_NBP_NAME_MAX * 4];
    const char *n = name ? name : "";
    if (!*n) {
        snprintf(err, err_len, "%s is required", what);
        return -1;
    }
    int len = macroman_from_utf8(n, mac, sizeof(mac));
    if (len < 0) {
        // Name the first character MacRoman lacks (or the bad byte).
        const uint8_t *p = (const uint8_t *)n;
        while (*p) {
            size_t cl = (*p < 0x80) ? 1 : (*p >= 0xF0) ? 4 : (*p >= 0xE0) ? 3 : (*p >= 0xC0) ? 2 : 1;
            char one[5] = {0};
            for (size_t i = 0; i < cl && p[i]; i++)
                one[i] = (char)p[i];
            uint8_t b;
            if (macroman_from_utf8(one, &b, 1) != 1) {
                snprintf(err, err_len, "%s '%s': '%s' cannot be written in MacRoman, the Mac's character set", what, n,
                         one);
                return -1;
            }
            p += strlen(one);
        }
        snprintf(err, err_len, "%s '%s' cannot be written in MacRoman", what, n);
        return -1;
    }
    if (len > ATALK_NBP_NAME_MAX) {
        snprintf(err, err_len, "%s max %d characters ('%s' is %d)", what, ATALK_NBP_NAME_MAX, n, len);
        return -1;
    }
    return 0;
}

// Enumerators tell apart entities on one socket, 1..255 (Inside AppleTalk 7-8).
static bool nbp_enumerator_in_use(uint32_t e, const void *ctx) {
    uint8_t socket = *(const uint8_t *)ctx;
    for (int i = 0; i < NBP_MAX_ENTRIES; i++) {
        const atalk_nbp_entry_t *entry = &g_net.nbp_entries[i];
        if (entry->in_use && entry->socket == socket && entry->enumerator == e)
            return true;
    }
    return false;
}

static uint8_t nbp_alloc_enumerator(uint8_t socket) {
    uint32_t cursor = g_net.nbp_next_enum, e = 1;
    // Cannot fail: at most NBP_MAX_ENTRIES of 255 are held.
    atalk_id_alloc(&cursor, 1, 255, nbp_enumerator_in_use, &socket, &e);
    g_net.nbp_next_enum = (uint8_t)cursor;
    return (uint8_t)e;
}

static bool nbp_field_equals_ci(const char *lhs, uint8_t lhs_len, const char *rhs, uint8_t rhs_len) {
    if (lhs_len != rhs_len)
        return false;
    for (uint8_t i = 0; i < lhs_len; i++) {
        if (macroman_fold((uint8_t)lhs[i]) != macroman_fold((uint8_t)rhs[i]))
            return false;
    }
    return true;
}

static bool nbp_entry_conflicts(const atalk_nbp_entry_t *candidate, int skip_index) {
    for (int i = 0; i < NBP_MAX_ENTRIES; i++) {
        if (i == skip_index)
            continue;
        const atalk_nbp_entry_t *existing = &g_net.nbp_entries[i];
        if (!existing->in_use)
            continue;
        if (!nbp_field_equals_ci(existing->object, existing->object_len, candidate->object, candidate->object_len))
            continue;
        if (!nbp_field_equals_ci(existing->type, existing->type_len, candidate->type, candidate->type_len))
            continue;
        if (!nbp_field_equals_ci(existing->zone, existing->zone_len, candidate->zone, candidate->zone_len))
            continue;
        return true;
    }
    return false;
}

static int nbp_populate_entry(atalk_nbp_entry_t *dst, const atalk_nbp_service_desc_t *desc) {
    if (!dst || !desc || !desc->object || !desc->type || desc->socket == 0)
        return -1;
    atalk_nbp_entry_t temp;
    memset(&temp, 0, sizeof(temp));
    temp.net = desc->net;
    temp.node = desc->node ? desc->node : ATALK_HOST_NODE;
    temp.socket = desc->socket;

    int obj_len = nbp_copy_field(temp.object, sizeof(temp.object), desc->object, false);
    if (obj_len < 0)
        return -1;
    temp.object_len = (uint8_t)obj_len;

    int type_len = nbp_copy_field(temp.type, sizeof(temp.type), desc->type, false);
    if (type_len < 0)
        return -1;
    temp.type_len = (uint8_t)type_len;

    const char *zone_src = (desc->zone && desc->zone[0]) ? desc->zone : "*";
    int zone_len = nbp_copy_field(temp.zone, sizeof(temp.zone), zone_src, false);
    if (zone_len < 0)
        return -1;
    temp.zone_len = (uint8_t)zone_len;

    *dst = temp;
    return 0;
}

static int nbp_register(const atalk_nbp_service_desc_t *desc, atalk_nbp_entry_t **out_entry) {
    if (!desc)
        return -1;
    int free_slot = -1;
    for (int i = 0; i < NBP_MAX_ENTRIES; i++) {
        if (!g_net.nbp_entries[i].in_use) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) {
        LOG(1, "NBP: registry full, cannot register '%s:%s'", desc->object ? desc->object : "",
            desc->type ? desc->type : "");
        return -1;
    }

    atalk_nbp_entry_t candidate;
    if (nbp_populate_entry(&candidate, desc) != 0)
        return -1;
    candidate.enumerator = nbp_alloc_enumerator(candidate.socket);
    candidate.in_use = true;

    if (nbp_entry_conflicts(&candidate, -1)) {
        LOG(2, "NBP: name conflict for '%s:%s@%s'", candidate.object, candidate.type, candidate.zone);
        return -1;
    }

    g_net.nbp_entries[free_slot] = candidate;
    if (out_entry)
        *out_entry = &g_net.nbp_entries[free_slot];
    LOG(3, "NBP register: object='%s' type='%s' zone='%s' socket=%u enum=%u", candidate.object, candidate.type,
        candidate.zone, (unsigned)candidate.socket, (unsigned)candidate.enumerator);
    return 0;
}

static int nbp_update(atalk_nbp_entry_t *entry, const atalk_nbp_service_desc_t *desc) {
    int idx = nbp_entry_index(entry);
    if (idx < 0 || !desc)
        return -1;

    atalk_nbp_entry_t candidate;
    if (nbp_populate_entry(&candidate, desc) != 0)
        return -1;
    candidate.in_use = true;
    if (candidate.socket == g_net.nbp_entries[idx].socket)
        candidate.enumerator = g_net.nbp_entries[idx].enumerator;
    else
        candidate.enumerator = nbp_alloc_enumerator(candidate.socket);

    if (nbp_entry_conflicts(&candidate, idx))
        return -1;

    g_net.nbp_entries[idx] = candidate;
    return 0;
}

static int nbp_unregister(atalk_nbp_entry_t *entry) {
    int idx = nbp_entry_index(entry);
    if (idx < 0)
        return -1;
    if (!g_net.nbp_entries[idx].in_use)
        return 0;
    LOG(3, "NBP unregister: object='%s' type='%s'", g_net.nbp_entries[idx].object, g_net.nbp_entries[idx].type);
    memset(&g_net.nbp_entries[idx], 0, sizeof(g_net.nbp_entries[idx]));
    return 0;
}

int atalk_nbp_publish(atalk_nbp_entry_t **entry, const atalk_nbp_service_desc_t *desc) {
    if (!entry || !desc)
        return -1;
    return *entry ? nbp_update(*entry, desc) : nbp_register(desc, entry);
}

void atalk_nbp_withdraw(atalk_nbp_entry_t **entry) {
    if (!entry || !*entry)
        return;
    nbp_unregister(*entry);
    *entry = NULL;
}

static bool nbp_pattern_is_all(const uint8_t *field, int len) {
    return (len == 1 && field[0] == '=');
}

static bool nbp_zone_query_is_wildcard(const nbp_tuple_t *tuple) {
    return (tuple->zone_len == 0) || (tuple->zone_len == 1 && tuple->zone[0] == '*');
}

// Iterative two-pointer glob matcher with backtracking.  NBP_APPROX_CHAR
// (`≈`) is a `*`-style wildcard matching any run of bytes.  The recursive
// formulation was O(2^n) for patterns with many wildcards (a crafted NBP
// lookup tuple could DoS the server).  This version is O(n*m) worst-case.
// Both sides are MacRoman; case folds by Inside AppleTalk Table D-2.
static bool nbp_glob_match_ci(const char *value, int value_len, const uint8_t *pattern, int pat_len) {
    int v = 0;
    int p = 0;
    int star_p = -1;
    int star_v = 0;
    while (v < value_len) {
        if (p < pat_len && pattern[p] == NBP_APPROX_CHAR) {
            // Collapse consecutive wildcards into a single backtrack point.
            while (p < pat_len && pattern[p] == NBP_APPROX_CHAR)
                p++;
            star_p = p;
            star_v = v;
            continue;
        }
        if (p < pat_len && macroman_fold((uint8_t)value[v]) == macroman_fold(pattern[p])) {
            v++;
            p++;
            continue;
        }
        if (star_p >= 0) {
            // Backtrack: let the wildcard consume one more byte and retry.
            p = star_p;
            star_v++;
            v = star_v;
            continue;
        }
        return false;
    }
    // Skip trailing wildcards on the pattern side.
    while (p < pat_len && pattern[p] == NBP_APPROX_CHAR)
        p++;
    return p == pat_len;
}

static bool nbp_field_matches(const char *value, uint8_t value_len, const uint8_t *pattern, int pattern_len) {
    if (pattern_len <= 0)
        return value_len == 0;
    if (nbp_pattern_is_all(pattern, pattern_len))
        return true;
    return nbp_glob_match_ci(value, value_len, pattern, pattern_len);
}

static bool nbp_zone_matches(const nbp_tuple_t *query, const atalk_nbp_entry_t *entry) {
    if (nbp_zone_query_is_wildcard(query))
        return true;
    if (entry->zone_len == 1 && entry->zone[0] == '*')
        return true; // default zone matches all queries
    return nbp_field_matches(entry->zone, entry->zone_len, query->zone, query->zone_len);
}

static void nbp_build_tuple_from_entry(const atalk_nbp_entry_t *entry, nbp_tuple_t *tuple) {
    memset(tuple, 0, sizeof(*tuple));
    tuple->net = entry->net;
    tuple->node = entry->node;
    tuple->socket = entry->socket;
    tuple->enumerator = entry->enumerator;
    tuple->object_len = entry->object_len;
    memcpy(tuple->object, entry->object, entry->object_len);
    tuple->type_len = entry->type_len;
    memcpy(tuple->type, entry->type, entry->type_len);
    tuple->zone_len = entry->zone_len;
    memcpy(tuple->zone, entry->zone, entry->zone_len);
}

static void nbp_send_reply_batch(atalk_conn_t *c, const ddp_header_t *request, uint8_t nbp_id,
                                 const nbp_tuple_t *tuples, int count) {
    if (count <= 0)
        return;
    ddp_header_t reply;
    nbp_header_t nbp_header;
    ddp_setup_reply(request, &reply);
    nbp_header.function = NBP_LKUP_REPLY;
    nbp_header.tuple_count = (count > 15) ? 15 : count;
    nbp_header.nbp_id = nbp_id;
    nbp_send(c, &reply, &nbp_header, tuples);
}

static void nbp_handle_lookup_tuple(atalk_conn_t *c, const ddp_header_t *request, uint8_t nbp_id,
                                    const nbp_tuple_t *query) {
    if (!request || !query)
        return;
    nbp_tuple_t batch[NBP_MAX_TUPLES_PER_PACKET];
    int batch_len = 0;

    for (int i = 0; i < NBP_MAX_ENTRIES; i++) {
        const atalk_nbp_entry_t *entry = &g_net.nbp_entries[i];
        if (!entry->in_use)
            continue;
        if (!nbp_zone_matches(query, entry))
            continue;
        if (!nbp_field_matches(entry->type, entry->type_len, query->type, query->type_len))
            continue;
        if (!nbp_field_matches(entry->object, entry->object_len, query->object, query->object_len))
            continue;
        nbp_build_tuple_from_entry(entry, &batch[batch_len++]);
        if (batch_len == NBP_MAX_TUPLES_PER_PACKET) {
            nbp_send_reply_batch(c, request, nbp_id, batch, batch_len);
            batch_len = 0;
        }
    }
    if (batch_len > 0)
        nbp_send_reply_batch(c, request, nbp_id, batch, batch_len);
}

static void nbp_dispatch(atalk_conn_t *c, const ddp_header_t *ddp_header, const nbp_header_t *header,
                         nbp_tuple_t *tuples, int tuple_count) {
    if (!header || !ddp_header)
        return;
    switch (header->function) {
    case NBP_BRRQ:
    case NBP_LKUP:
    case NBP_FWDREQ:
        for (int i = 0; i < tuple_count; i++)
            nbp_handle_lookup_tuple(c, ddp_header, header->nbp_id, &tuples[i]);
        break;
    case NBP_LKUP_REPLY:
        // Replies to a lookup we issued (the PPC browse is the only client).
        nbp_deliver_lookup_reply(c, header->nbp_id, tuples, tuple_count);
        break;
    default:
        atalk_drop(c, ATALK_DROP_UNHANDLED, "NBP function %d", (int)header->function);
        break;
    }
}

static void nbp_parse_and_dispatch(atalk_conn_t *c, const ddp_header_t *ddp, const uint8_t *buf, size_t len) {
    if (!ddp || !buf)
        return;
    if (len < 2) {
        atalk_drop(c, ATALK_DROP_MALFORMED, "NBP packet of %zu bytes", len);
        return;
    }
    nbp_header_t header;
    nbp_tuple_t tuples[16]; // the tuple count is a 4-bit field: at most 15
    int parsed = 0;

    const uint8_t *p = buf;
    int rem = (int)len;
    header.function = (p[0] >> 4) & 0x0F;
    header.tuple_count = p[0] & 0x0F;
    header.nbp_id = p[1];
    p += 2;
    rem -= 2;

    for (int i = 0; i < header.tuple_count && parsed < (int)ARRAY_LEN(tuples); i++) {
        if (rem < 8)
            break;
        tuples[parsed].net = (uint16_t)((p[0] << 8) | p[1]);
        tuples[parsed].node = p[2];
        tuples[parsed].socket = p[3];
        tuples[parsed].enumerator = p[4];
        p += 5;
        rem -= 5;
        if (!nbp_parse_pstr32(&p, &rem, tuples[parsed].object, sizeof(tuples[parsed].object),
                              &tuples[parsed].object_len))
            break;
        if (!nbp_parse_pstr32(&p, &rem, tuples[parsed].type, sizeof(tuples[parsed].type), &tuples[parsed].type_len))
            break;
        if (!nbp_parse_pstr32(&p, &rem, tuples[parsed].zone, sizeof(tuples[parsed].zone), &tuples[parsed].zone_len))
            break;
        parsed++;
    }
    // A packet whose tuples run out before its count does is not a shorter
    // packet: drop it rather than act on the part that parsed.
    if (parsed != header.tuple_count) {
        atalk_drop(c, ATALK_DROP_MALFORMED, "NBP packet claims %d tuples, %d parse", (int)header.tuple_count, parsed);
        return;
    }

    nbp_dispatch(c, ddp, &header, tuples, parsed);
}

static void nbp_send(atalk_conn_t *c, const ddp_header_t *ddp_header, const nbp_header_t *nbp_header,
                     const nbp_tuple_t *nbp_tuple) {
    uint8_t buffer[DDP_MAX_DATA_SIZE];
    int size = 0;

#define NBP_ENSURE(nbytes)                                                                                             \
    do {                                                                                                               \
        if (size + (int)(nbytes) > DDP_MAX_DATA_SIZE) {                                                                \
            LOG(2, "NBP reply too large (%d) – refusing", size + (int)(nbytes));                                       \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

    NBP_ENSURE(2);
    buffer[size++] = (uint8_t)(((nbp_header->function & 0x0F) << 4) | (nbp_header->tuple_count & 0x0F));
    buffer[size++] = nbp_header->nbp_id;

    for (int i = 0; i < nbp_header->tuple_count; i++) {
        NBP_ENSURE(5);
        buffer[size++] = (uint8_t)((nbp_tuple[i].net >> 8) & 0xFF);
        buffer[size++] = (uint8_t)(nbp_tuple[i].net & 0xFF);
        buffer[size++] = nbp_tuple[i].node;
        buffer[size++] = nbp_tuple[i].socket;
        buffer[size++] = nbp_tuple[i].enumerator;

        int o_len = nbp_tuple[i].object_len;
        NBP_ENSURE(1 + o_len);
        buffer[size++] = (uint8_t)o_len;
        if (o_len) {
            memcpy(&buffer[size], nbp_tuple[i].object, (size_t)o_len);
            size += o_len;
        }

        int t_len = nbp_tuple[i].type_len;
        NBP_ENSURE(1 + t_len);
        buffer[size++] = (uint8_t)t_len;
        if (t_len) {
            memcpy(&buffer[size], nbp_tuple[i].type, (size_t)t_len);
            size += t_len;
        }

        int z_len = nbp_tuple[i].zone_len;
        NBP_ENSURE(1 + z_len);
        buffer[size++] = (uint8_t)z_len;
        if (z_len) {
            memcpy(&buffer[size], nbp_tuple[i].zone, (size_t)z_len);
            size += z_len;
        }
    }

    const char *fn_name = nbp_function_name(nbp_header->function);
    if (fn_name) {
        LOG(5, "NBP -> Mac %s: tuples=%d nbpId=%u bytes=%d", fn_name, nbp_header->tuple_count,
            (unsigned)nbp_header->nbp_id, size);
    } else {
        LOG(5, "NBP -> Mac func=%d: tuples=%d nbpId=%u bytes=%d", nbp_header->function, nbp_header->tuple_count,
            (unsigned)nbp_header->nbp_id, size);
    }
    LOG_INDENT(4);
    ddp_send(c, ddp_header, buffer, size);
    LOG_INDENT(-4);

#undef NBP_ENSURE
}

// === Outgoing lookups (object model: the PPC browse) ========================

// Hand every tuple of a matching reply to the waiting caller.  Replies to a
// broadcast trickle in one packet per responder, so the slot stays armed
// until the caller issues another lookup.
static void nbp_deliver_lookup_reply(atalk_conn_t *c, uint8_t nbp_id, const nbp_tuple_t *tuples, int count) {
    if (!c->nbp_lookup.active || c->nbp_lookup.nbp_id != nbp_id || !c->nbp_lookup.cb)
        return;
    for (int i = 0; i < count; i++) {
        atalk_nbp_info_t info;
        memset(&info, 0, sizeof(info));
        // MacRoman on the wire, UTF-8 to the caller.
        macroman_to_utf8(tuples[i].object, (size_t)tuples[i].object_len, info.object, sizeof(info.object));
        macroman_to_utf8(tuples[i].type, (size_t)tuples[i].type_len, info.type, sizeof(info.type));
        macroman_to_utf8(tuples[i].zone, (size_t)tuples[i].zone_len, info.zone, sizeof(info.zone));
        info.net = tuples[i].net;
        info.node = tuples[i].node;
        info.socket = tuples[i].socket;
        LOG(4, "NBP reply: '%s:%s@%s' at %u:%u", info.object, info.type, info.zone, info.node, info.socket);
        c->nbp_lookup.cb(c->nbp_lookup.ctx, &info);
    }
}

int atalk_nbp_lookup(const char *object, const char *type, const char *zone, uint8_t reply_socket,
                     atalk_nbp_reply_fn cb, void *ctx) {
    atalk_conn_t *c = g_net.plugged;
    if (!object || !type || !cb || reply_socket == 0)
        return -1;
    if (!c || !c->enabled)
        return -1;

    // The pattern goes out in MacRoman, as every NBP name does: a UTF-8 "≈"
    // becomes the wildcard byte $C5.
    char obj_mac[NBP_OBJECT_MAX + 1], type_mac[NBP_TYPE_MAX + 1], zone_mac[NBP_ZONE_MAX + 1];
    const char *zone_str = (zone && zone[0]) ? zone : "*";
    int obj_n = nbp_copy_field(obj_mac, sizeof(obj_mac), object, true);
    int type_n = nbp_copy_field(type_mac, sizeof(type_mac), type, true);
    int zone_n = nbp_copy_field(zone_mac, sizeof(zone_mac), zone_str, true);
    if (obj_n < 0 || type_n < 0 || zone_n < 0)
        return -1;
    size_t obj_len = (size_t)obj_n, type_len = (size_t)type_n, zone_len = (size_t)zone_n;

    // The tuple of a lookup names where the replies should go, then the
    // pattern being looked up.
    uint8_t buf[2 + 5 + 3 * 33];
    int n = 0;
    buf[n++] = (uint8_t)((NBP_LKUP << 4) | 1);
    c->nbp_next_lookup_id = (uint8_t)(c->nbp_next_lookup_id == 255 ? 1 : c->nbp_next_lookup_id + 1);
    buf[n++] = c->nbp_next_lookup_id;
    buf[n++] = 0; // net high
    buf[n++] = 0; // net low
    buf[n++] = ATALK_HOST_NODE;
    buf[n++] = reply_socket;
    buf[n++] = 0; // enumerator
    buf[n++] = (uint8_t)obj_len;
    memcpy(&buf[n], obj_mac, obj_len);
    n += (int)obj_len;
    buf[n++] = (uint8_t)type_len;
    memcpy(&buf[n], type_mac, type_len);
    n += (int)type_len;
    buf[n++] = (uint8_t)zone_len;
    memcpy(&buf[n], zone_mac, zone_len);
    n += (int)zone_len;

    c->nbp_lookup.active = true;
    c->nbp_lookup.nbp_id = c->nbp_next_lookup_id;
    c->nbp_lookup.cb = cb;
    c->nbp_lookup.ctx = ctx;

    // NBP runs on socket 2 of every node; the lookup goes to the broadcast
    // node so every machine on the segment answers.
    atalk_socket_addr_t dest = {.net = 0, .node = 0xFF, .socket = 2};
    LOG(4, "NBP lookup '%s:%s@%s' (id=%u, replies to socket %u)", object, type, zone_str,
        (unsigned)c->nbp_next_lookup_id, (unsigned)reply_socket);
    return atalk_ddp_send_to(&dest, reply_socket, DDP_NBP, buf, n);
}

void atalk_nbp_lookup_cancel(void) {
    atalk_conn_t *c = g_net.plugged;
    if (c)
        memset(&c->nbp_lookup, 0, sizeof(c->nbp_lookup));
}

static void nbp_in(atalk_conn_t *c, ddp_header_t *ddp, const uint8_t *buf, size_t len) {
    c->stats.nbp_packets++;
    uint8_t header_byte = (len >= 1) ? buf[0] : 0;
    uint8_t nbp_id = (len >= 2) ? buf[1] : 0;
    int function = (header_byte >> 4) & 0x0F;
    int tuple_count = header_byte & 0x0F;
    const char *fn_name = nbp_function_name(function);
    if (fn_name) {
        LOG(4, "NBP <- Mac %s: tuples=%d nbpId=%u len=%zu", fn_name, tuple_count, (unsigned)nbp_id, len);
    } else {
        LOG(4, "NBP <- Mac func=%d: tuples=%d nbpId=%u len=%zu", function, tuple_count, (unsigned)nbp_id, len);
    }
    LOG_INDENT(4);
    nbp_parse_and_dispatch(c, ddp, buf, len);
    LOG_INDENT(-4);
}

// =============================== ATP (AppleTalk Transaction Protocol) ===============================

// Utility helpers -----------------------------------------------------------
static void atp_retry_timeout_cb(void *source, uint64_t data);
static void atp_release_timeout_cb(void *source, uint64_t data);
static int parse_atp(const uint8_t *buf, int len, atp_packet_t *atp);

static uint64_t atp_ms_to_ns(uint32_t ms) {
    if (ms == 0)
        return (uint64_t)ATP_DEFAULT_RETRY_MS * 1000000ULL;
    return (uint64_t)ms * 1000000ULL;
}

static uint64_t atp_seconds_to_ns(uint32_t seconds) {
    return (uint64_t)seconds * 1000000000ULL;
}

static uint32_t atp_trel_hint_seconds(uint8_t hint) {
    switch (hint & 0x07) {
    case 0:
        return 30;
    case 1:
        return 60;
    case 2:
        return 120;
    case 3:
        return 240;
    case 4:
        return 480;
    default:
        return 30;
    }
}

// A timer event's data: the slot's generation in the high 32 bits, its index
// (an outgoing request, or an XO cache entry) in the low 16.  The index is a
// uint16_t, so the tables it indexes must stay under 65,536 slots.
static_assert(ATP_MAX_OUTGOING <= 0xFFFF && ATP_MAX_XO_CACHE <= 0xFFFF, "ATP slot index must fit 16 bits");

static uint64_t atp_encode_event_data(uint16_t index, uint32_t generation) {
    return ((uint64_t)generation << 32) | (uint64_t)index;
}

static bool atp_decode_event_data(uint64_t data, uint16_t *index, uint32_t *generation) {
    if (!index || !generation)
        return false;
    *index = (uint16_t)(data & 0xFFFFu);
    *generation = (uint32_t)(data >> 32);
    return true;
}

// Called when the connection is plugged in, never lazily at first arm: a
// checkpoint taken with an ATP transaction in flight -- any AFP command, any
// print job -- replays its events into the restored machine's scheduler.
static void atp_timers_init(atalk_conn_t *c) {
    atalk_timer_init(c, &c->atp_retry_timer, "atp", "retry_timeout", &atp_retry_timeout_cb);
    atalk_timer_init(c, &c->atp_release_timer, "atp", "xo_release", &atp_release_timeout_cb);
}

// Handler registry ---------------------------------------------------------
static atp_handler_slot_t *atp_find_handler_slot(uint8_t socket) {
    for (int i = 0; i < ATP_MAX_HANDLERS; i++) {
        if (g_net.atp_handlers[i].in_use && g_net.atp_handlers[i].socket == socket)
            return &g_net.atp_handlers[i];
    }
    return NULL;
}

int atp_register_socket_handler(uint8_t socket, const atp_socket_handler_t *handler, void *ctx) {
    if (!handler || !handler->handle_request)
        return -1;
    atp_handler_slot_t *existing = atp_find_handler_slot(socket);
    if (existing) {
        existing->handler = *handler;
        existing->ctx = ctx;
        return 0;
    }
    for (int i = 0; i < ATP_MAX_HANDLERS; i++) {
        if (!g_net.atp_handlers[i].in_use) {
            g_net.atp_handlers[i].in_use = true;
            g_net.atp_handlers[i].socket = socket;
            g_net.atp_handlers[i].handler = *handler;
            g_net.atp_handlers[i].ctx = ctx;
            return 0;
        }
    }
    LOG_ATP(1, "ATP: handler table full, cannot register socket %u", (unsigned)socket);
    return -1;
}

void atp_unregister_socket_handler(uint8_t socket) {
    atp_handler_slot_t *slot = atp_find_handler_slot(socket);
    if (slot)
        slot->in_use = false;
}

// Request ID generator -----------------------------------------------------
// A TID is taken while another outstanding request from the same socket
// holds it (the request being numbered is already in the table).
typedef struct {
    const atalk_conn_t *conn;
    uint8_t socket;
    const atp_request_handle_t *self;
} atp_tid_scope_t;

static bool atp_tid_in_use(uint32_t tid, const void *ctx) {
    const atp_tid_scope_t *scope = ctx;
    for (int i = 0; i < ATP_MAX_OUTGOING; i++) {
        const atp_request_handle_t *r = &scope->conn->atp_requests[i];
        if (r != scope->self && r->in_use && r->src_socket == scope->socket && r->tid == tid)
            return true;
    }
    return false;
}

static atp_request_handle_t *atp_alloc_request_slot(atalk_conn_t *c) {
    for (int i = 0; i < ATP_MAX_OUTGOING; i++) {
        if (!c->atp_requests[i].in_use) {
            memset(&c->atp_requests[i], 0, sizeof(c->atp_requests[i]));
            c->atp_requests[i].in_use = true;
            return &c->atp_requests[i];
        }
    }
    return NULL;
}

static void atp_request_complete(atalk_conn_t *c, atp_request_handle_t *req, atp_request_result_t result) {
    if (!req || !req->in_use)
        return;
    // Cancel any pending retry timer
    uint16_t index = (uint16_t)(req - c->atp_requests);
    atalk_timer_cancel(&c->atp_retry_timer, atp_encode_event_data(index, req->timer_generation));
    req->timer_generation++;
    // The callback gets a copy, not the slot: once in_use clears, a submit
    // from inside the callback may take this very slot.  The handle it is
    // passed is only good for comparing against one it stored.
    atp_request_callbacks_t callbacks = req->callbacks;
    void *cb_ctx = req->cb_ctx;
    req->in_use = false;
    if (callbacks.on_complete)
        callbacks.on_complete(req, result, cb_ctx);
}

static void atp_send_trel(atalk_conn_t *c, const atp_request_handle_t *req) {
    if (!req || !req->xo)
        return;
    uint8_t buffer[8] = {0};
    // TRel packets should have the lower 3 bits set to zero (Inside AppleTalk, p. 9-13)
    buffer[0] = ATP_CONTROL_TREL;
    buffer[2] = (uint8_t)((req->tid >> 8) & 0xFF);
    buffer[3] = (uint8_t)(req->tid & 0xFF);
    ddp_header_t ddp;
    memset(&ddp, 0, sizeof(ddp));
    ddp.llap.dst = req->dest.node;
    ddp.llap.src = ATALK_HOST_NODE;
    ddp.llap.type = LLAP_DDP_SHORT;
    ddp.len = (uint16_t)(DDP_SHORT_HEADER_SIZE + sizeof(buffer));
    ddp.dst_socket = req->dest.socket;
    ddp.src_socket = req->src_socket;
    ddp.type = DDP_ATP;
    ddp_send(c, &ddp, buffer, sizeof(buffer));
    LOG_ATP(3, "ATP: sent TRel tid=0x%04X srcSock=%u dstSock=%u", req->tid, (unsigned)req->src_socket,
            (unsigned)req->dest.socket);
}

static void atp_send_request_packets(atalk_conn_t *c, atp_request_handle_t *req, uint8_t bitmap) {
    uint8_t atp_buf[DDP_MAX_DATA_SIZE];
    atp_buf[0] = req->base_ctl;
    atp_buf[1] = bitmap;
    atp_buf[2] = (uint8_t)((req->tid >> 8) & 0xFF);
    atp_buf[3] = (uint8_t)(req->tid & 0xFF);
    memcpy(&atp_buf[4], req->user, 4);
    if (req->payload_len > 0)
        memcpy(&atp_buf[8], req->payload, (size_t)req->payload_len);
    int total = 8 + req->payload_len;

    ddp_header_t ddp;
    memset(&ddp, 0, sizeof(ddp));
    ddp.llap.dst = req->dest.node;
    ddp.llap.src = ATALK_HOST_NODE;
    ddp.llap.type = LLAP_DDP_SHORT;
    ddp.len = (uint16_t)(total + DDP_SHORT_HEADER_SIZE);
    ddp.dst_socket = req->dest.socket;
    ddp.src_socket = req->src_socket;
    ddp.type = DDP_ATP;
    ddp_send(c, &ddp, atp_buf, total);
    LOG_ATP(3, "ATP: sent TReq tid=0x%04X srcSock=%u dstSock=%u bitmap=0x%02X", req->tid, (unsigned)req->src_socket,
            (unsigned)req->dest.socket, (unsigned)bitmap);
}

// Cancel-then-arm is not atomic: it relies on the scheduler running every
// event and every AppleTalk entry point on the one worker thread, so no
// retry can fire between the cancel and the generation bump.  Revisit (with
// the other unsynchronized stack state) if the scheduler ever goes threaded.
static void atp_arm_retry_timer(atalk_conn_t *c, atp_request_handle_t *req) {
    uint16_t index = (uint16_t)(req - c->atp_requests);
    // Cancel any existing retry event before scheduling a new one
    atalk_timer_cancel(&c->atp_retry_timer, atp_encode_event_data(index, req->timer_generation));
    req->timer_generation++;
    LOG_ATP(5, "ATP: arm retry timer tid=0x%04X timeout_ns=%" PRIu64, req->tid, req->retry_timeout_ns);
    atalk_timer_arm(&c->atp_retry_timer, atp_encode_event_data(index, req->timer_generation), req->retry_timeout_ns);
}

static void atp_retry_request(atalk_conn_t *c, atp_request_handle_t *req, bool consume_retry) {
    if (!req || req->pending_bitmap == 0)
        return;
    if (!req->infinite_retries && consume_retry) {
        if (req->retries_remaining == 0) {
            LOG_ATP(2, "ATP: retries exhausted for tid=0x%04X", req->tid);
            atp_send_trel(c, req);
            atp_request_complete(c, req, ATP_REQUEST_RESULT_TIMEOUT);
            return;
        }
        req->retries_remaining--;
    }
    c->stats.atp_retries++;
    atp_send_request_packets(c, req, req->pending_bitmap);
    atp_arm_retry_timer(c, req);
}

static void atp_retry_timeout_cb(void *source, uint64_t data) {
    atalk_conn_t *c = CONN_OF(source, atp_retry_timer);
    uint16_t index;
    uint32_t generation;
    if (!atp_decode_event_data(data, &index, &generation))
        return;
    if (index >= ATP_MAX_OUTGOING)
        return;
    atp_request_handle_t *req = &c->atp_requests[index];
    if (!req->in_use || req->timer_generation != generation)
        return;
    LOG_ATP(3, "ATP: retry timeout tid=0x%04X bitmap=0x%02X", req->tid, (unsigned)req->pending_bitmap);
    atp_retry_request(c, req, true);
}

atp_request_handle_t *atp_request_submit(const atp_request_params_t *params, const atp_request_callbacks_t *callbacks,
                                         void *ctx) {
    atalk_conn_t *c = g_net.plugged;
    if (!c || !params || params->bitmap == 0)
        return NULL; // nobody is on the cable to ask
    if (params->payload_len < 0 || params->payload_len > ATP_MAX_ATP_PAYLOAD)
        return NULL;
    atp_request_handle_t *req = atp_alloc_request_slot(c);
    if (!req)
        return NULL;

    req->src_socket = params->src_socket;
    req->dest = params->dest;
    req->initial_bitmap = params->bitmap;
    req->pending_bitmap = params->bitmap;
    req->payload_len = params->payload_len;
    if (req->payload_len > 0 && params->payload)
        memcpy(req->payload, params->payload, (size_t)req->payload_len);
    memcpy(req->user, params->user, sizeof(req->user));
    req->retry_timeout_ns = atp_ms_to_ns(params->retry_timeout_ms);
    req->retries_remaining = (params->retry_limit > 0) ? params->retry_limit : 0;
    req->infinite_retries = (params->retry_limit < 0);
    req->callbacks = callbacks ? *callbacks : (atp_request_callbacks_t){0};
    req->cb_ctx = ctx;
    req->xo = (params->mode == ATP_TRANSACTION_XO);
    req->trel_hint = params->trel_timer_hint & 0x07;
    req->base_ctl = ATP_CONTROL_TREQ;
    if (req->xo) {
        req->base_ctl |= ATP_CONTROL_XO;
        req->base_ctl |= req->trel_hint;
    }
    atp_tid_scope_t scope = {.conn = c, .socket = req->src_socket, .self = req};
    uint32_t tid = 0;
    if (!atalk_id_alloc(&c->next_tid, 0, 0xFFFF, atp_tid_in_use, &scope, &tid)) {
        req->in_use = false; // cannot happen: at most ATP_MAX_OUTGOING of 65,536 are held
        return NULL;
    }
    req->tid = (uint16_t)tid;

    c->stats.atp_requests++;
    atp_send_request_packets(c, req, req->pending_bitmap);
    atp_arm_retry_timer(c, req);
    return req;
}

void atp_request_cancel(atp_request_handle_t *handle) {
    // A live request belongs to the plugged connection: unplugging one
    // drops its requests.
    atalk_conn_t *c = g_net.plugged;
    if (!c || !handle || !handle->in_use)
        return;
    LOG_ATP(3, "ATP: cancel request tid=0x%04X", handle->tid);
    atp_request_complete(c, handle, ATP_REQUEST_RESULT_ABORTED);
}

// XO cache helpers ---------------------------------------------------------
static int atp_xo_find(atalk_conn_t *c, uint16_t tid, uint8_t requester_node, uint8_t requester_socket,
                       uint8_t responder_socket) {
    for (int i = 0; i < ATP_MAX_XO_CACHE; i++) {
        if (!c->xo_entries[i].in_use)
            continue;
        if (c->xo_entries[i].tid == tid && c->xo_entries[i].requester_node == requester_node &&
            c->xo_entries[i].requester_socket == requester_socket &&
            c->xo_entries[i].responder_socket == responder_socket) {
            return i;
        }
    }
    return -1;
}

// Forward declaration for atp_xo_alloc
static void atp_xo_schedule_release(atalk_conn_t *c, atp_xo_entry_t *entry);

static int atp_xo_alloc(atalk_conn_t *c, const ddp_header_t *ddp, const atp_packet_t *atp) {
    for (int i = 0; i < ATP_MAX_XO_CACHE; i++) {
        if (!c->xo_entries[i].in_use) {
            memset(&c->xo_entries[i], 0, sizeof(c->xo_entries[i]));
            c->xo_entries[i].in_use = true;
            c->xo_entries[i].tid = atp->tid;
            c->xo_entries[i].requester_node = ddp->llap.src;
            c->xo_entries[i].requester_socket = ddp->src_socket;
            c->xo_entries[i].responder_socket = ddp->dst_socket;
            c->xo_entries[i].trel_hint = (uint8_t)(atp->ctl & 0x07);
            // Start release timer immediately (Inside AppleTalk p. 9-17)
            atp_xo_schedule_release(c, &c->xo_entries[i]);
            LOG_ATP(10, "ATP: XO alloc slot=%d tid=0x%04X node=%u sock=%u->%u trel=%u", i, atp->tid, ddp->llap.src,
                    ddp->src_socket, ddp->dst_socket, atp->ctl & 0x07);
            return i;
        }
    }
    LOG_ATP(2, "ATP: XO cache full (tid=0x%04X node=%u sock=%u)", atp->tid, ddp->llap.src, ddp->src_socket);
    return -1;
}

static void atp_xo_free(atalk_conn_t *c, atp_xo_entry_t *entry) {
    if (!entry)
        return;
    uint16_t index = (uint16_t)(entry - c->xo_entries);
    LOG_ATP(10, "ATP: XO free slot=%u tid=0x%04X", index, entry->tid);
    // Cancel pending release timer event
    atalk_timer_cancel(&c->atp_release_timer, atp_encode_event_data(index, entry->release_generation));
    entry->in_use = false;
    entry->release_generation++;
}

// Drop every outgoing request and every XO cache entry, and their timers.
// When the stack is only being detached from the link its clients are still
// up, so each outstanding request completes as ABORTED and they clean up.
// When the connection is unplugged they are already gone: requests are
// dropped without a callback.  The socket handlers are the network's and stay.
static void atp_reset(atalk_conn_t *c, bool teardown) {
    for (int i = 0; i < ATP_MAX_OUTGOING; i++) {
        atp_request_handle_t *req = &c->atp_requests[i];
        if (!req->in_use)
            continue;
        if (!teardown) {
            atp_request_complete(c, req, ATP_REQUEST_RESULT_ABORTED);
        } else {
            atalk_timer_cancel(&c->atp_retry_timer, atp_encode_event_data((uint16_t)i, req->timer_generation));
            req->timer_generation++;
            req->in_use = false;
        }
    }
    for (int i = 0; i < ATP_MAX_XO_CACHE; i++)
        if (c->xo_entries[i].in_use)
            atp_xo_free(c, &c->xo_entries[i]);
}

static void atp_xo_store_packet(atp_xo_entry_t *entry, uint8_t seq, const uint8_t *bytes, int len) {
    if (!entry || seq >= ATP_MAX_RESPONSE_FRAGMENTS || len <= 0 || len > DDP_MAX_DATA_SIZE)
        return;
    atp_resp_packet_cache_t *slot = &entry->packets[seq];
    slot->valid = true;
    slot->seq = seq;
    slot->len = len;
    memcpy(slot->bytes, bytes, (size_t)len);
}

static void atp_xo_schedule_release(atalk_conn_t *c, atp_xo_entry_t *entry) {
    if (!entry)
        return;
    uint16_t index = (uint16_t)(entry - c->xo_entries);
    // Cancel any existing release event for this entry before scheduling a new one
    atalk_timer_cancel(&c->atp_release_timer, atp_encode_event_data(index, entry->release_generation));
    entry->release_generation++;
    uint32_t seconds = atp_trel_hint_seconds(entry->trel_hint);
    atalk_timer_arm(&c->atp_release_timer, atp_encode_event_data(index, entry->release_generation),
                    atp_seconds_to_ns(seconds));
}

static void atp_release_timeout_cb(void *source, uint64_t data) {
    atalk_conn_t *c = CONN_OF(source, atp_release_timer);
    uint16_t index;
    uint32_t generation;
    if (!atp_decode_event_data(data, &index, &generation))
        return;
    if (index >= ATP_MAX_XO_CACHE)
        return;
    atp_xo_entry_t *entry = &c->xo_entries[index];
    if (!entry->in_use || entry->release_generation != generation)
        return;
    LOG_ATP(3, "ATP: XO release timeout tid=0x%04X", entry->tid);
    atp_xo_free(c, entry);
}

// Retransmit cached XO response packets matching the bitmap
static void atp_xo_send_cached(atalk_conn_t *c, atp_xo_entry_t *entry, const ddp_header_t *ddp, uint8_t bitmap) {
    if (!entry || !entry->response_ready)
        return;
    LOG_ATP(6, "ATP: XO retransmit cached tid=0x%04X bitmap=0x%02X", entry->tid, bitmap);
    ddp_header_t reply;
    ddp_setup_reply(ddp, &reply);
    reply.type = DDP_ATP;
    if (bitmap == 0x00) {
        // bitmap=0x00: peer received all packets, resend the last (EOM) packet
        for (int i = ATP_MAX_RESPONSE_FRAGMENTS - 1; i >= 0; i--) {
            if (entry->packets[i].valid) {
                ddp_send(c, &reply, entry->packets[i].bytes, entry->packets[i].len);
                break;
            }
        }
    } else {
        // Normal: resend only the packets requested by the bitmap
        for (int i = 0; i < ATP_MAX_RESPONSE_FRAGMENTS; i++) {
            atp_resp_packet_cache_t *slot = &entry->packets[i];
            if (!slot->valid)
                continue;
            if (!(bitmap & (1u << slot->seq)))
                continue;
            ddp_send(c, &reply, slot->bytes, slot->len);
        }
    }
    atp_xo_schedule_release(c, entry);
}

// Outgoing response helpers -------------------------------------------------
int atp_responder_send_packets(const ddp_header_t *request_ddp, const atp_packet_t *request_atp,
                               const atp_response_packet_desc_t *packets, size_t packet_count) {
    atalk_conn_t *c = g_net.plugged;
    if (!c || !request_ddp || !request_atp || !packets || packet_count == 0)
        return -1;
    if (packet_count > ATP_MAX_RESPONSE_FRAGMENTS)
        return -1;
    // All or nothing: check every packet before the first goes out, so a bad
    // one late in the burst cannot leave the guest half a response (and an
    // XO entry that never becomes ready).
    for (size_t i = 0; i < packet_count; i++) {
        if (packets[i].payload_len < 0 || packets[i].payload_len > (DDP_MAX_DATA_SIZE - 8))
            return -1;
    }

    ddp_header_t reply;
    ddp_setup_reply(request_ddp, &reply);
    reply.type = DDP_ATP;
    bool xo = (request_atp->ctl & ATP_CONTROL_XO) != 0;
    int xo_index = -1;
    if (xo) {
        xo_index =
            atp_xo_find(c, request_atp->tid, request_ddp->llap.src, request_ddp->src_socket, request_ddp->dst_socket);
        if (xo_index < 0)
            xo_index = atp_xo_alloc(c, request_ddp, request_atp);
    }

    for (size_t i = 0; i < packet_count; i++) {
        const atp_response_packet_desc_t *desc = &packets[i];
        uint8_t buffer[DDP_MAX_DATA_SIZE];
        // XO bit is only meaningful on TReq; do not set it on TResp
        uint8_t ctl = ATP_CONTROL_TRESP;
        if (desc->sts)
            ctl |= ATP_CONTROL_STS;
        bool is_last = (i == packet_count - 1);
        if (desc->eom || is_last)
            ctl |= ATP_CONTROL_EOM;
        buffer[0] = ctl;
        buffer[1] = (uint8_t)i;
        buffer[2] = (uint8_t)((request_atp->tid >> 8) & 0xFF);
        buffer[3] = (uint8_t)(request_atp->tid & 0xFF);
        if (desc->user)
            memcpy(&buffer[4], desc->user, 4);
        else
            memcpy(&buffer[4], request_atp->user, 4);
        if (desc->payload_len > 0 && desc->payload)
            memcpy(&buffer[8], desc->payload, (size_t)desc->payload_len);
        int total = 8 + desc->payload_len;
        ddp_send(c, &reply, buffer, total);
        if (xo_index >= 0)
            atp_xo_store_packet(&c->xo_entries[xo_index], (uint8_t)i, buffer, total);
    }
    if (xo_index >= 0) {
        c->xo_entries[xo_index].response_ready = true;
        atp_xo_schedule_release(c, &c->xo_entries[xo_index]);
    }
    return (int)packet_count;
}

int atp_responder_send_simple(const ddp_header_t *request_ddp, const atp_packet_t *request_atp, const uint8_t user[4],
                              const uint8_t *payload, int payload_len, bool sts) {
    if (!request_atp)
        return -1;
    uint8_t fallback_user[4];
    if (!user)
        memcpy(fallback_user, request_atp->user, sizeof(fallback_user));
    atp_response_packet_desc_t desc = {
        .payload = payload, .payload_len = payload_len, .user = user ? user : fallback_user, .sts = sts, .eom = true};
    return atp_responder_send_packets(request_ddp, request_atp, &desc, 1);
}

static int parse_atp(const uint8_t *buf, int len, atp_packet_t *atp) {
    if (!buf || len < 8 || !atp)
        return -1;
    atp->ctl = buf[0];
    atp->bitmap = buf[1];
    atp->tid = (uint16_t)((buf[2] << 8) | buf[3]);
    memcpy(atp->user, &buf[4], 4);
    atp->data = &buf[8];
    atp->data_len = len - 8;
    return 0;
}

// Incoming response handling -----------------------------------------------
static atp_request_handle_t *atp_match_request(atalk_conn_t *c, const ddp_header_t *ddp, const atp_packet_t *atp) {
    for (int i = 0; i < ATP_MAX_OUTGOING; i++) {
        atp_request_handle_t *req = &c->atp_requests[i];
        if (!req->in_use)
            continue;
        if (req->tid != atp->tid)
            continue;
        if (req->src_socket != ddp->dst_socket)
            continue;
        if (req->dest.socket != ddp->src_socket || req->dest.node != ddp->llap.src || req->dest.net != ddp->src_net)
            continue;
        return req;
    }
    return NULL;
}

static void atp_handle_response(atalk_conn_t *c, const ddp_header_t *ddp, const atp_packet_t *atp) {
    atp_request_handle_t *req = atp_match_request(c, ddp, atp);
    if (!req) {
        atalk_drop(c, ATALK_DROP_UNHANDLED, "ATP response tid %04X matches no request", (unsigned)atp->tid);
        return;
    }
    uint8_t seq = atp->bitmap & 0x07;
    uint8_t mask = (uint8_t)(1u << seq);
    bool duplicate = ((req->pending_bitmap & mask) == 0);
    if (!duplicate) {
        req->pending_bitmap &= (uint8_t)~mask;
        if (atp->ctl & ATP_CONTROL_EOM) {
            uint8_t higher = (uint8_t)(mask - 1u);
            req->pending_bitmap &= higher;
        }
        if (req->callbacks.on_response) {
            atp_response_fragment_t fragment = {.seq = seq,
                                                .duplicate = false,
                                                .eom = (atp->ctl & ATP_CONTROL_EOM) != 0,
                                                .sts = (atp->ctl & ATP_CONTROL_STS) != 0,
                                                .bitmap_remaining = req->pending_bitmap,
                                                .data = atp->data,
                                                .data_len = atp->data_len};
            memcpy(fragment.user, atp->user, sizeof(fragment.user));
            req->callbacks.on_response(&fragment, req->cb_ctx);
        }
    }
    if (atp->ctl & ATP_CONTROL_STS) {
        atp_retry_request(c, req, false);
        return;
    }
    if (req->pending_bitmap == 0) {
        atp_send_trel(c, req);
        atp_request_complete(c, req, ATP_REQUEST_RESULT_OK);
    } else if (!duplicate) {
        // Re-arm the retry timer on each valid response so it doesn't fire
        // while the Mac is still actively sending response packets.
        atp_arm_retry_timer(c, req);
    }
}

static void atp_handle_trel(atalk_conn_t *c, const ddp_header_t *ddp, const atp_packet_t *atp) {
    int idx = atp_xo_find(c, atp->tid, ddp->llap.src, ddp->src_socket, ddp->dst_socket);
    LOG_ATP(10, "ATP: TRel tid=0x%04X node=%u sock=%u xo_slot=%d", atp->tid, ddp->llap.src, ddp->src_socket, idx);
    if (idx >= 0)
        atp_xo_free(c, &c->xo_entries[idx]);
}

// Request dispatch ---------------------------------------------------------
static void atp_dispatch_registered_request(atalk_conn_t *c, const ddp_header_t *ddp, atp_packet_t *atp) {
    atp_handler_slot_t *slot = atp_find_handler_slot(ddp->dst_socket);
    if (!slot) {
        atalk_drop(c, ATALK_DROP_UNHANDLED, "ATP request for socket %u", (unsigned)ddp->dst_socket);
        return;
    }
    bool xo = (atp->ctl & ATP_CONTROL_XO) != 0;
    if (xo) {
        int existing = atp_xo_find(c, atp->tid, ddp->llap.src, ddp->src_socket, ddp->dst_socket);
        if (existing >= 0) {
            atp_xo_send_cached(c, &c->xo_entries[existing], ddp, atp->bitmap);
            return;
        }
        if (atp_xo_alloc(c, ddp, atp) < 0)
            LOG_ATP(2, "ATP: XO cache full, duplicate protection degraded");
    }
    slot->handler.handle_request(ddp, atp, slot->ctx);
}

static void atp_in(atalk_conn_t *c, const ddp_header_t *ddp, const uint8_t *buf, int len) {
    atp_packet_t atp;
    if (parse_atp(buf, len, &atp) != 0) {
        atalk_drop(c, ATALK_DROP_MALFORMED, "ATP packet of %d bytes", len);
        return;
    }
    uint8_t ctl_type = (uint8_t)(atp.ctl & 0xC0);

    if (ctl_type == ATP_CONTROL_TREL) {
        atp_handle_trel(c, ddp, &atp);
        return;
    }
    if (ctl_type == ATP_CONTROL_TRESP) {
        atp_handle_response(c, ddp, &atp);
        return;
    }
    if (ctl_type != ATP_CONTROL_TREQ) {
        atalk_drop(c, ATALK_DROP_MALFORMED, "ATP control byte %02X", (unsigned)atp.ctl);
        return;
    }

    atp_dispatch_registered_request(c, ddp, &atp);
}
