// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_asp.c
// ASP, the AppleTalk Session Protocol (Inside AppleTalk ch. 11) -- the server
// end, on ATP.  Sessions, tickle expiry, attentions, and the two-transaction
// SPWrite; what a command means is the client's business (appletalk_asp.h).
//
// This lived inside appletalk.c and called the AFP server by name: the one
// place in the stack where the transport depended on the application above it
// (10-network F-21).  It now has ADSP's shape -- a client registered by the
// layer above -- so ASP can be driven, and tested, without AFP.
//
// Every packet but GetStatus and OpenSess names its session by the one-byte
// session id in ATP user byte 1 (Inside AppleTalk Fig. 11-10).  The session is
// found in one place, asp_find, which also requires the packet to come from
// the node that opened it.

#include "appletalk_asp.h"

#include "afp_wire.h"
#include "appletalk.h"
#include "appletalk_internal.h"
#include "atalk_id.h"
#include "common.h"
#include "log.h"
#include "scheduler.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("appletalk");

// SPFunction, ATP user byte 0 (Inside AppleTalk 11-26)
#define ASP_CLOSE_SESS     1
#define ASP_COMMAND        2
#define ASP_GET_STAT       3
#define ASP_OPEN_SESS      4
#define ASP_TICKLE         5
#define ASP_WRITE          6
#define ASP_WRITE_CONTINUE 7
#define ASP_ATTENTION      8

// The one ASP version (OpenSess user bytes 2-3)
#define ASP_VERSION 0x0100

// OpenSessReply error codes (Inside AppleTalk Appendix C)
#define ASP_ERR_BAD_VERS_NUM 0xFBD6u // -1066
#define ASP_ERR_SERVER_BUSY  0xFBD1u // -1071

// ASP requires a workstation to tickle every 30 seconds; a server may close a
// session after two minutes of silence (Inside AppleTalk ch. 11).  Expiry runs
// on a scheduler event so a guest that is reset — never sending CloseSess —
// still gets its forks and locks back.
#define ASP_SESSION_TIMEOUT_NS (120ULL * 1000000000ULL)
#define ASP_SESSION_SWEEP_NS   (15ULL * 1000000000ULL)

// ASP SPWrite: the command arrives first, and the server fetches the data
// with a WriteContinue request to the workstation.  The factor of 8 matches
// the ATP burst size cap.
#define ASP_WRITE_QUANTUM (8 * ATP_MAX_ATP_PAYLOAD) // 4624 bytes max per WriteContinue

typedef struct asp_session asp_session_t;

// An SPWrite waiting for its data.  Allocated when the Write arrives, freed
// when its WriteContinue completes.
typedef struct {
    asp_session_t *owner; // NULL once the session is gone: complete silently
    atp_request_handle_t *request; // the WriteContinue
    ddp_header_t orig_ddp; // the Write, answered when the data is in
    atp_packet_t orig_atp;
    uint8_t orig_atp_data[ATP_MAX_ATP_PAYLOAD];
    uint8_t afp_opcode;
    uint8_t afp_params[ATP_MAX_ATP_PAYLOAD];
    int afp_params_len;
    uint8_t write_buf[ASP_WRITE_QUANTUM];
    int write_len;
} asp_write_t;

struct asp_session {
    bool in_use;
    uint16_t sess_ref; // the client's key for the session, and the object model's
    uint8_t sess_id; // the one-byte id on the wire, unique among live sessions
    uint8_t wss; // workstation session socket (OpenSess user byte 1)
    uint8_t client_node; // LLAP node of the workstation
    uint64_t last_activity_ns; // guest time of the last packet from this client
    asp_write_t *write; // an SPWrite waiting for its WriteContinue
};

#define MAX_ASP_SESS 4
static asp_session_t g_sessions[MAX_ASP_SESS];
static uint32_t g_next_sess_ref = 0x0021; // atalk_id_alloc cursors
static uint32_t g_next_sess_id = 1;

static void asp_in(const ddp_header_t *ddp, atp_packet_t *atp, void *ctx);
static void asp_arm_session_sweep(void);
static void asp_sweep_cb(void *source, uint64_t data);
static atalk_timer_t g_asp_sweep_timer; // idle-session expiry, armed while any session is open

// === The client ===============================================================

static const asp_client_t *g_client;
static void *g_client_ctx;

void asp_set_client(const asp_client_t *client, void *ctx) {
    g_client = client;
    g_client_ctx = client ? ctx : NULL;
}

static uint32_t asp_client_command(uint16_t session_ref, uint8_t opcode, const uint8_t *in, int in_len, uint8_t *out,
                                   int out_max, int *out_len) {
    *out_len = 0;
    if (!g_client || !g_client->on_command)
        return AFPERR_CallNotSupported; // nothing serves commands
    return g_client->on_command(g_client_ctx, session_ref, opcode, in, in_len, out, out_max, out_len);
}

// === Sessions ===================================================================

// Guest time, not host time, so an instruction-budgeted run expires sessions
// deterministically.
static uint64_t asp_now_ns(void) {
    scheduler_t *sched = atalk_scheduler();
    return sched ? (uint64_t)scheduler_time_ns(sched) : 0;
}

static bool sess_ref_in_use(uint32_t ref, const void *ctx) {
    (void)ctx;
    for (int i = 0; i < MAX_ASP_SESS; i++)
        if (g_sessions[i].in_use && g_sessions[i].sess_ref == ref)
            return true;
    return false;
}

static bool sess_id_in_use(uint32_t id, const void *ctx) {
    (void)ctx;
    for (int i = 0; i < MAX_ASP_SESS; i++)
        if (g_sessions[i].in_use && g_sessions[i].sess_id == id)
            return true;
    return false;
}

// The session a packet from `ddp` names by `sess_id`.  The id is one byte by
// spec ("a unique (per SLS) 1-byte session ID", 11-24): it is allocated unique
// among live sessions, and a packet must come from the node that opened the
// session.  It used to be the low byte of a 16-bit counter, so two live
// sessions 256 opens apart shared it, and any node could drive any session by
// guessing one byte (10-network F-06).
static asp_session_t *asp_find(uint8_t sess_id, const ddp_header_t *ddp) {
    for (int i = 0; i < MAX_ASP_SESS; i++) {
        asp_session_t *s = &g_sessions[i];
        if (s->in_use && s->sess_id == sess_id && s->client_node == ddp->llap.src)
            return s;
    }
    return NULL;
}

static asp_session_t *asp_by_ref(uint16_t ref) {
    for (int i = 0; i < MAX_ASP_SESS; i++)
        if (g_sessions[i].in_use && g_sessions[i].sess_ref == ref)
            return &g_sessions[i];
    return NULL;
}

// Close a session, however it ends -- CloseSess, expiry, the stack detaching.
// A write waiting for its data is abandoned without an answer (its client is
// gone), and the client lets go of the forks, locks and snapshots the session
// held.
static void asp_session_release(asp_session_t *s) {
    if (!s->in_use)
        return;
    uint16_t ref = s->sess_ref;
    asp_write_t *w = s->write;
    s->write = NULL;
    s->in_use = false;
    if (w) {
        w->owner = NULL;
        atp_request_cancel(w->request); // completes it; asp_wc_on_complete frees it
    }
    if (g_client && g_client->on_close)
        g_client->on_close(g_client_ctx, ref);
}

// Session numbering travels in the stack's checkpoint record.
uint16_t asp_next_ref(void) {
    return (uint16_t)g_next_sess_ref;
}
void asp_set_next_ref(uint16_t ref) {
    if (ref)
        g_next_sess_ref = ref;
}

// === ASP session views, attention and expiry (WP-8) =========================

// Session-table accessors for `appletalk.afp.sessions`.
int atalk_asp_session_max(void) {
    return MAX_ASP_SESS;
}

bool atalk_asp_session_in_use(int index) {
    return index >= 0 && index < MAX_ASP_SESS && g_sessions[index].in_use;
}

bool atalk_asp_session_info(int index, atalk_session_info_t *out) {
    if (!atalk_asp_session_in_use(index) || !out)
        return false;
    const asp_session_t *s = &g_sessions[index];
    memset(out, 0, sizeof(*out));
    out->session_ref = s->sess_ref;
    out->client_node = s->client_node;
    out->socket = s->wss;
    const char *ver =
        (g_client && g_client->session_version) ? g_client->session_version(g_client_ctx, s->sess_ref) : NULL;
    snprintf(out->afp_version, sizeof(out->afp_version), "%s", ver ? ver : "");
    out->open_forks = (g_client && g_client->open_forks) ? g_client->open_forks(g_client_ctx, s->sess_ref) : 0;
    uint64_t now = asp_now_ns();
    out->idle_ns = (now > s->last_activity_ns) ? (now - s->last_activity_ns) : 0;
    return true;
}

// Send an ASP Attention to one session.  The code travels in the ATP user
// bytes of a server-originated request to the workstation session socket
// (Inside AppleTalk ch. 11; AFP_21_22 Table 1-7 for the code values).
int atalk_asp_send_attention(uint16_t session_ref, uint16_t code) {
    asp_session_t *s = asp_by_ref(session_ref);
    if (!s || !s->client_node || !s->wss)
        return -1;
    atp_request_params_t params = {
        .dest = {.net = 0, .node = s->client_node, .socket = s->wss},
        .src_socket = HOST_AFP_SOCKET,
        .bitmap = 0x01, // one response packet is enough for an attention
        .mode = ATP_TRANSACTION_ALO,
        .trel_timer_hint = 0,
        .user = {ASP_ATTENTION, s->sess_id, (uint8_t)(code >> 8), (uint8_t)code},
        .payload = NULL,
        .payload_len = 0,
        .retry_timeout_ms = 2000,
        .retry_limit = 2,
    };
    LOG(3, "ASP Attention: session=0x%02X code=0x%04X → node=%u socket=%u", s->sess_id, code, s->client_node, s->wss);
    return atp_request_submit(&params, NULL, NULL) ? 0 : -1;
}

void atalk_asp_broadcast_attention(uint16_t code) {
    for (int i = 0; i < MAX_ASP_SESS; i++)
        if (g_sessions[i].in_use)
            atalk_asp_send_attention(g_sessions[i].sess_ref, code);
}

void atalk_asp_close_all_sessions(void) {
    for (int i = 0; i < MAX_ASP_SESS; i++)
        asp_session_release(&g_sessions[i]);
}

// Close every session that has gone quiet past the ASP timeout, returning
// their forks, locks and desktop references.
static void asp_expire_sessions(void) {
    uint64_t now = asp_now_ns();
    for (int i = 0; i < MAX_ASP_SESS; i++) {
        asp_session_t *s = &g_sessions[i];
        if (!s->in_use || now < s->last_activity_ns + ASP_SESSION_TIMEOUT_NS)
            continue;
        LOG(1, "ASP: session 0x%04X expired after %llu s of silence", s->sess_ref,
            (unsigned long long)((now - s->last_activity_ns) / 1000000000ULL));
        asp_session_release(s);
    }
}

// Scheduler callback: expire stale sessions and re-arm while any remain.
static void asp_sweep_cb(void *source, uint64_t data) {
    (void)source;
    (void)data;
    asp_expire_sessions();
    asp_arm_session_sweep();
}

static void asp_arm_session_sweep(void) {
    bool any = false;
    for (int i = 0; i < MAX_ASP_SESS; i++)
        any |= g_sessions[i].in_use;
    if (!any)
        return; // nothing to watch; the next OpenSess re-arms
    atalk_timer_arm(&g_asp_sweep_timer, 0, ASP_SESSION_SWEEP_NS);
}

// === Replies =====================================================================

// Answer a request with `user` and `data`, split across as many response
// packets as the request's bitmap allows.
static void asp_reply(const ddp_header_t *ddp, const atp_packet_t *atp, const uint8_t user[4], const uint8_t *data,
                      int len) {
    if (len <= ATP_MAX_ATP_PAYLOAD) {
        atp_responder_send_simple(ddp, atp, user, len > 0 ? data : NULL, len, false);
        return;
    }
    int max_packets = 0;
    for (uint8_t bm = atp->bitmap; bm & 1; bm >>= 1)
        max_packets++;
    if (max_packets < 1)
        max_packets = 1;
    if (max_packets > ATP_MAX_RESPONSE_FRAGMENTS)
        max_packets = ATP_MAX_RESPONSE_FRAGMENTS;
    int num_packets = (len + ATP_MAX_ATP_PAYLOAD - 1) / ATP_MAX_ATP_PAYLOAD;
    if (num_packets > max_packets)
        num_packets = max_packets;
    atp_response_packet_desc_t descs[ATP_MAX_RESPONSE_FRAGMENTS];
    int offset = 0;
    for (int i = 0; i < num_packets; i++) {
        int chunk = len - offset;
        if (chunk > ATP_MAX_ATP_PAYLOAD)
            chunk = ATP_MAX_ATP_PAYLOAD;
        descs[i].payload = data + offset;
        descs[i].payload_len = chunk;
        descs[i].user = user;
        descs[i].sts = false;
        descs[i].eom = (i == num_packets - 1);
        offset += chunk;
    }
    atp_responder_send_packets(ddp, atp, descs, (size_t)num_packets);
}

// A command's reply: the 32-bit result in the four user bytes (CmdResult).
static void asp_reply_result(const ddp_header_t *ddp, const atp_packet_t *atp, uint32_t result, const uint8_t *data,
                             int len) {
    uint8_t user[4];
    WR_BE32(user, result);
    asp_reply(ddp, atp, user, result == AFPERR_NoErr ? data : NULL, result == AFPERR_NoErr ? len : 0);
}

// === SPWrite ======================================================================

// Called for each ATP response fragment from the client's WriteContinueReply
static void asp_wc_on_response(const atp_response_fragment_t *fragment, void *ctx) {
    asp_write_t *w = (asp_write_t *)ctx;
    if (!w || !fragment || !fragment->data)
        return;
    int avail = ASP_WRITE_QUANTUM - w->write_len;
    int copy = (fragment->data_len < avail) ? fragment->data_len : avail;
    if (copy > 0) {
        memcpy(w->write_buf + w->write_len, fragment->data, (size_t)copy);
        w->write_len += copy;
    }
}

// The WriteContinue finished: run the write and answer the original Write --
// unless the session has gone, in which case nobody is listening.
static void asp_wc_on_complete(atp_request_handle_t *handle, atp_request_result_t result, void *ctx) {
    (void)handle;
    asp_write_t *w = (asp_write_t *)ctx;
    if (!w)
        return;
    asp_session_t *s = w->owner;
    if (!s) {
        free(w);
        return;
    }
    s->write = NULL;

    uint32_t afp_result;
    uint8_t afp_out[ATP_MAX_ATP_PAYLOAD];
    int afp_len = 0;
    if (result != ATP_REQUEST_RESULT_OK || w->write_len <= 0) {
        LOG(2, "ASP WriteContinue failed: result=%d write_len=%d", (int)result, w->write_len);
        afp_result = AFPERR_MiscErr;
    } else {
        // The command's parameters, then the data (FPWrite: 11 bytes + data)
        int combined_len = w->afp_params_len + w->write_len;
        uint8_t *combined = (uint8_t *)malloc((size_t)combined_len);
        if (!combined) {
            afp_result = AFPERR_MiscErr;
        } else {
            memcpy(combined, w->afp_params, (size_t)w->afp_params_len);
            memcpy(combined + w->afp_params_len, w->write_buf, (size_t)w->write_len);
            afp_result = asp_client_command(s->sess_ref, w->afp_opcode, combined, combined_len, afp_out,
                                            (int)sizeof(afp_out), &afp_len);
            free(combined);
        }
    }
    LOG(6, "ASP Write complete: opcode=0x%02X result=0x%08X replyLen=%d dataLen=%d", w->afp_opcode, afp_result, afp_len,
        w->write_len);
    asp_reply_result(&w->orig_ddp, &w->orig_atp, afp_result, afp_out, afp_len);
    free(w);
}

static const atp_request_callbacks_t g_asp_wc_callbacks = {
    .on_response = asp_wc_on_response,
    .on_complete = asp_wc_on_complete,
};

// SPWrite: hold the command, fetch the data with a WriteContinue, answer when
// it is in.  One write per session at a time; a second one while the first
// waits is answered with an error rather than dropped.  There used to be one
// pending write for the whole server: a second session's Write got no reply
// at all, and its XO entry swallowed the retransmissions for 30 s (10-network
// F-14).
static void asp_write(asp_session_t *s, const ddp_header_t *ddp, const atp_packet_t *atp) {
    const uint8_t *cmd = atp->data;
    int cmd_len = atp->data_len;
    uint8_t opcode = (cmd_len > 0) ? cmd[0] : 0;
    LOG(6, "ASP Write: session=0x%02X seq=0x%04X opcode=0x%02X len=%d", s->sess_id, (unsigned)RD_BE16(&atp->user[2]),
        opcode, cmd_len);
    if (s->write) {
        LOG(2, "ASP Write: session 0x%02X already has a write waiting for its data", s->sess_id);
        asp_reply_result(ddp, atp, AFPERR_MiscErr, NULL, 0);
        return;
    }
    asp_write_t *w = (asp_write_t *)calloc(1, sizeof(*w));
    if (!w) {
        asp_reply_result(ddp, atp, AFPERR_MiscErr, NULL, 0);
        return;
    }
    w->owner = s;
    w->orig_ddp = *ddp;
    w->orig_atp = *atp;
    // The ATP data points into the receive buffer; keep a copy.
    if (atp->data && atp->data_len > 0) {
        int dlen = (atp->data_len > (int)sizeof(w->orig_atp_data)) ? (int)sizeof(w->orig_atp_data) : atp->data_len;
        memcpy(w->orig_atp_data, atp->data, (size_t)dlen);
        w->orig_atp.data = w->orig_atp_data;
        w->orig_atp.data_len = dlen;
    }
    w->afp_opcode = opcode;
    if (cmd_len > 1) {
        w->afp_params_len = cmd_len - 1;
        if (w->afp_params_len > (int)sizeof(w->afp_params))
            w->afp_params_len = (int)sizeof(w->afp_params);
        memcpy(w->afp_params, cmd + 1, (size_t)w->afp_params_len);
    }

    uint8_t wc_data[2];
    WR_BE16(wc_data, ASP_WRITE_QUANTUM);
    atp_request_params_t wc_params = {
        .dest = {.net = 0, .node = s->client_node, .socket = s->wss},
        .src_socket = HOST_AFP_SOCKET,
        .bitmap = 0xFF, // request up to 8 response packets
        .mode = ATP_TRANSACTION_XO,
        .trel_timer_hint = 0,
        .user = {ASP_WRITE_CONTINUE, s->sess_id, atp->user[2], atp->user[3]},
        .payload = wc_data,
        .payload_len = 2,
        .retry_timeout_ms = 5000,
        .retry_limit = 10,
    };
    LOG(6, "ASP WriteContinue: node=%u wss=%u bufSize=%u", s->client_node, s->wss, ASP_WRITE_QUANTUM);
    s->write = w;
    w->request = atp_request_submit(&wc_params, &g_asp_wc_callbacks, w);
    if (!w->request) {
        LOG(2, "ASP WriteContinue: failed to submit ATP request");
        s->write = NULL;
        free(w);
        asp_reply_result(ddp, atp, AFPERR_MiscErr, NULL, 0);
    }
    // otherwise the answer is deferred until the WriteContinue completes
}

// === The server socket ===========================================================

// SPGetStatus: the service status block, whatever the request carried.  It
// used to hand a GetStatus that carried data to the AFP dispatcher as a
// command from session 0 -- a second, session-free command channel
// (10-network F-05).
static void asp_get_status(const ddp_header_t *ddp, const atp_packet_t *atp) {
    LOG(3, "ASP GetStatus: request from node=%u socket=%u", ddp->llap.src, ddp->src_socket);
    uint8_t *block = NULL;
    size_t block_len = 0;
    int rc = (g_client && g_client->get_status) ? g_client->get_status(g_client_ctx, &block, &block_len) : -1;
    static const uint8_t zero[4] = {0, 0, 0, 0};
    int len = (rc == 0 && block) ? (int)(block_len > ATP_MAX_ATP_PAYLOAD ? ATP_MAX_ATP_PAYLOAD : block_len) : 0;
    atp_responder_send_simple(ddp, atp, zero, len > 0 ? block : NULL, len, false);
    free(block);
}

// OpenSess: user bytes [4, WSS, version]; the reply's are [SSS, SessID, error]
// (Inside AppleTalk 11-26).  A refusal is by the spec's codes: BadVersNum for
// a version other than 1.0, ServerBusy when the table is full or the client
// will not take the session (the AFP server is disabled).  A full table used
// to answer "1", no ASP code at all (10-network N-04).
static void asp_open_session(const ddp_header_t *ddp, const atp_packet_t *atp) {
    uint8_t wss = atp->user[1];
    uint16_t version = RD_BE16(&atp->user[2]);
    uint16_t err = 0;
    asp_session_t *s = NULL;
    uint32_t ref = 0, id = 0;
    if (version != ASP_VERSION) {
        err = ASP_ERR_BAD_VERS_NUM;
    } else {
        for (int i = 0; i < MAX_ASP_SESS && !s; i++)
            if (!g_sessions[i].in_use)
                s = &g_sessions[i];
        if (!s || !atalk_id_alloc(&g_next_sess_ref, 1, 0xFFFF, sess_ref_in_use, NULL, &ref) ||
            !atalk_id_alloc(&g_next_sess_id, 1, 255, sess_id_in_use, NULL, &id)) {
            err = ASP_ERR_SERVER_BUSY;
            s = NULL;
        } else if (g_client && g_client->on_open && !g_client->on_open(g_client_ctx, (uint16_t)ref)) {
            err = ASP_ERR_SERVER_BUSY;
            s = NULL;
        }
    }
    uint8_t user[4] = {0, 0, 0, 0};
    if (s) {
        memset(s, 0, sizeof(*s));
        s->in_use = true;
        s->sess_ref = (uint16_t)ref;
        s->sess_id = (uint8_t)id;
        s->wss = wss;
        s->client_node = ddp->llap.src;
        s->last_activity_ns = asp_now_ns();
        asp_arm_session_sweep();
        user[0] = HOST_AFP_SOCKET;
        user[1] = s->sess_id;
        LOG(3, "ASP OpenSess: id=0x%02X ref=0x%04X node=%u wss=%u", s->sess_id, s->sess_ref, s->client_node, wss);
    } else {
        WR_BE16(&user[2], err);
        LOG(1, "ASP OpenSess from node %u refused: %s", ddp->llap.src,
            err == ASP_ERR_BAD_VERS_NUM ? "unknown ASP version" : "server busy");
    }
    atp_responder_send_simple(ddp, atp, user, NULL, 0, false);
}

static void asp_in(const ddp_header_t *ddp, atp_packet_t *atp, void *ctx) {
    (void)ctx;
    if (!ddp || !atp)
        return;
    uint8_t func = atp->user[0];
    if (func == ASP_GET_STAT) {
        asp_get_status(ddp, atp);
        return;
    }
    if (func == ASP_OPEN_SESS) {
        asp_open_session(ddp, atp);
        return;
    }

    static const uint8_t zero[4] = {0, 0, 0, 0};
    asp_session_t *s = asp_find(atp->user[1], ddp);
    if (!s) {
        switch (func) {
        case ASP_COMMAND:
        case ASP_WRITE:
            // A command on a session that is not open is answered, not run.
            // It used to run as "session 0", with no login (10-network F-05).
            LOG(2, "ASP: function %u for unknown session 0x%02X from node %u", func, atp->user[1], ddp->llap.src);
            asp_reply_result(ddp, atp, AFPERR_SessClosed, NULL, 0);
            return;
        case ASP_CLOSE_SESS:
            atp_responder_send_simple(ddp, atp, zero, NULL, 0, false); // already closed
            return;
        default:
            return; // a stray Tickle
        }
    }
    s->last_activity_ns = asp_now_ns(); // any traffic is liveness

    switch (func) {
    case ASP_TICKLE:
        // One-way keepalive (no response).  It is what keeps a session
        // alive; a client that stops tickling is expired by the sweep.
        return;
    case ASP_CLOSE_SESS:
        // The session is in user byte 1, and CloseSess carries no data
        // (Fig. 11-10).  This read a "session ref" from the ATP data, which
        // is always absent, so no CloseSess ever closed anything: every
        // unmount left the session held until the two-minute sweep, and with
        // four slots a fifth mount inside that window was refused
        // (10-network N-03).  The reply's user bytes are zero, and it carries
        // no data.
        LOG(3, "ASP CloseSess: session=0x%02X", s->sess_id);
        asp_session_release(s);
        atp_responder_send_simple(ddp, atp, zero, NULL, 0, false);
        return;
    case ASP_COMMAND: {
        const uint8_t *cmd = atp->data;
        int cmd_len = atp->data_len;
        uint8_t opcode = (cmd_len > 0) ? cmd[0] : 0;
        LOG(6, "ASP Command: session=0x%02X seq=0x%04X opcode=0x%02X len=%d", s->sess_id,
            (unsigned)RD_BE16(&atp->user[2]), opcode, cmd_len);
        int max_packets = 0;
        for (uint8_t bm = atp->bitmap; bm & 1; bm >>= 1)
            max_packets++;
        if (max_packets < 1)
            max_packets = 1;
        if (max_packets > ATP_MAX_RESPONSE_FRAGMENTS)
            max_packets = ATP_MAX_RESPONSE_FRAGMENTS;
        uint8_t afp_out[ATP_MAX_RESPONSE_FRAGMENTS * ATP_MAX_ATP_PAYLOAD];
        int afp_len = 0;
        uint32_t result =
            asp_client_command(s->sess_ref, opcode, (cmd_len > 0) ? (cmd + 1) : NULL, (cmd_len > 0) ? (cmd_len - 1) : 0,
                               afp_out, max_packets * ATP_MAX_ATP_PAYLOAD, &afp_len);
        LOG(6, "ASP Command result: opcode=0x%02X result=0x%08X replyLen=%d", opcode, result, afp_len);
        asp_reply_result(ddp, atp, result, afp_out, afp_len);
        return;
    }
    case ASP_WRITE:
        asp_write(s, ddp, atp);
        return;
    default:
        LOG(3, "ASP: unknown function %u on session 0x%02X", func, s->sess_id);
        atp_responder_send_simple(ddp, atp, zero, NULL, 0, false);
        return;
    }
}

// === Lifecycle ==================================================================

// Forget every session and every pending write without calling back: the
// machine is going away (its ATP requests are dropped with it), or coming up.
static void asp_reset(void) {
    for (int i = 0; i < MAX_ASP_SESS; i++)
        free(g_sessions[i].write);
    memset(g_sessions, 0, sizeof(g_sessions));
}

void asp_init(void) {
    asp_reset();
    if (atalk_scheduler())
        atalk_timer_init(&g_asp_sweep_timer, "asp", "session_sweep", &asp_sweep_cb);
    static const atp_socket_handler_t handler = {.handle_request = asp_in};
    atp_register_socket_handler(HOST_AFP_SOCKET, &handler, NULL);
    atp_register_socket_handler(HOST_AFP_COMPAT_SOCKET, &handler, NULL);
}

void asp_shutdown(void) {
    asp_reset();
}
