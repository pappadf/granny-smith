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

#include "appletalk_asp.h"

#include "appletalk.h"
#include "appletalk_internal.h"
#include "log.h"
#include "scheduler.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("appletalk");

static void asp_in(const ddp_header_t *ddp, atp_packet_t *atp, void *ctx);

// === The client ===============================================================

static const asp_client_t *g_client;
static void *g_client_ctx;

void asp_set_client(const asp_client_t *client, void *ctx) {
    g_client = client;
    g_client_ctx = client ? ctx : NULL;
}

static void asp_client_closed(uint16_t session_ref) {
    if (g_client && g_client->on_close)
        g_client->on_close(g_client_ctx, session_ref);
}

static uint32_t asp_client_command(uint16_t session_ref, uint8_t opcode, const uint8_t *in, int in_len, uint8_t *out,
                                   int out_max, int *out_len) {
    *out_len = 0;
    if (!g_client || !g_client->on_command)
        return 0xFFFFEC60u; // CallNotSupported: nothing serves commands
    return g_client->on_command(g_client_ctx, session_ref, opcode, in, in_len, out, out_max, out_len);
}

// ASP Commands / SPFunction (values per docs/core/network/appletalk.md)
#define ASP_CLOSE_SESS     1
#define ASP_COMMAND        2
#define ASP_GET_STAT       3
#define ASP_OPEN_SESS      4
#define ASP_TICKLE         5
#define ASP_WRITE          6
#define ASP_WRITE_CONTINUE 7
#define ASP_ATTENTION      8

typedef struct {
    bool in_use;
    uint16_t sess_ref; // 16-bit session reference (internal)
    uint8_t wss; // workstation session socket (from OpenSess request)
    uint8_t sss; // server session socket (returned in reply)
    uint8_t client_node; // LLAP node of the workstation
    uint16_t asp_version; // ASP version from OpenSess
    char afp_version[24]; // negotiated at FPLogin ("" until then)
    uint64_t last_activity_ns; // host time of the last packet from this client
    uint16_t next_attn_tid; // ATP transaction IDs for server-sent attentions
} asp_session_t;

#define MAX_ASP_SESS 4
static asp_session_t g_sessions[MAX_ASP_SESS];
static uint16_t g_next_sess_ref = 0x0021;

// ASP requires a workstation to tickle every 30 seconds; a server may close a
// session after two minutes of silence (Inside AppleTalk ch. 11).  Expiry runs
// on a scheduler event so a guest that is reset — never sending CloseSess —
// still gets its forks and locks back.
#define ASP_SESSION_TIMEOUT_NS (120ULL * 1000000000ULL)
#define ASP_SESSION_SWEEP_NS   (15ULL * 1000000000ULL)

static void asp_arm_session_sweep(void);
static void asp_sweep_cb(void *source, uint64_t data);
static atalk_timer_t g_asp_sweep_timer; // idle-session expiry, armed while any session is open

// Emulated-time reading used for tickle bookkeeping.  Guest time, not host
// time, so an instruction-budgeted integration run sees deterministic expiry.
static uint64_t asp_now_ns(void) {
    scheduler_t *sched = atalk_scheduler();
    return sched ? (uint64_t)scheduler_time_ns(sched) : 0;
}

static int alloc_session(void) {
    for (int i = 0; i < MAX_ASP_SESS; i++) {
        if (!g_sessions[i].in_use) {
            memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
            g_sessions[i].in_use = true;
            g_sessions[i].sess_ref = g_next_sess_ref++;
            g_sessions[i].sss = HOST_AFP_SOCKET; // default SSS
            g_sessions[i].last_activity_ns = asp_now_ns();
            g_sessions[i].next_attn_tid = 0x4000;
            asp_arm_session_sweep();
            return g_sessions[i].sess_ref;
        }
    }
    return -1;
}

static void free_session(uint16_t ref) {
    for (int i = 0; i < MAX_ASP_SESS; i++)
        if (g_sessions[i].in_use && g_sessions[i].sess_ref == ref) {
            // The client owns the forks, locks and enumeration snapshots
            // this session held; it must let them go with the session.
            asp_client_closed(ref);
            g_sessions[i].in_use = false;
            return;
        }
}

static asp_session_t *get_session(uint16_t ref) {
    for (int i = 0; i < MAX_ASP_SESS; i++)
        if (g_sessions[i].in_use && g_sessions[i].sess_ref == ref)
            return &g_sessions[i];
    return NULL;
}

// Session numbering travels in the stack's checkpoint record.
uint16_t asp_next_ref(void) {
    return g_next_sess_ref;
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
    snprintf(out->afp_version, sizeof(out->afp_version), "%s", s->afp_version);
    out->open_forks = (g_client && g_client->open_forks) ? g_client->open_forks(g_client_ctx, s->sess_ref) : 0;
    uint64_t now = asp_now_ns();
    out->idle_ns = (now > s->last_activity_ns) ? (now - s->last_activity_ns) : 0;
    return true;
}

void atalk_asp_session_set_afp_version(uint16_t session_ref, const char *version) {
    for (int i = 0; i < MAX_ASP_SESS; i++) {
        if (!g_sessions[i].in_use || g_sessions[i].sess_ref != session_ref)
            continue;
        snprintf(g_sessions[i].afp_version, sizeof(g_sessions[i].afp_version), "%s", version ? version : "");
        return;
    }
}

const char *atalk_asp_session_afp_version(uint16_t session_ref) {
    for (int i = 0; i < MAX_ASP_SESS; i++)
        if (g_sessions[i].in_use && g_sessions[i].sess_ref == session_ref)
            return g_sessions[i].afp_version;
    return NULL;
}

// Send an ASP Attention to one session.  The code travels in the ATP user
// bytes of a server-originated request to the workstation session socket
// (Inside AppleTalk ch. 11; AFP_21_22 Table 1-7 for the code values).
int atalk_asp_send_attention(uint16_t session_ref, uint16_t code) {
    for (int i = 0; i < MAX_ASP_SESS; i++) {
        asp_session_t *s = &g_sessions[i];
        if (!s->in_use || s->sess_ref != session_ref)
            continue;
        if (!s->client_node || !s->wss)
            return -1;
        atp_request_params_t params = {
            .dest = {.net = 0, .node = s->client_node, .socket = s->wss},
            .src_socket = HOST_AFP_SOCKET,
            .bitmap = 0x01, // one response packet is enough for an attention
            .mode = ATP_TRANSACTION_ALO,
            .trel_timer_hint = 0,
            .user = {ASP_ATTENTION, (uint8_t)(s->sess_ref & 0xFF), (uint8_t)(code >> 8), (uint8_t)code},
            .payload = NULL,
            .payload_len = 0,
            .retry_timeout_ms = 2000,
            .retry_limit = 2,
        };
        LOG(3, "ASP Attention: session=0x%02X code=0x%04X → node=%u socket=%u", s->sess_ref & 0xFF, code,
            s->client_node, s->wss);
        return atp_request_submit(&params, NULL, NULL) ? 0 : -1;
    }
    return -1;
}

void atalk_asp_broadcast_attention(uint16_t code) {
    for (int i = 0; i < MAX_ASP_SESS; i++)
        if (g_sessions[i].in_use)
            atalk_asp_send_attention(g_sessions[i].sess_ref, code);
}

void atalk_asp_close_all_sessions(void) {
    for (int i = 0; i < MAX_ASP_SESS; i++) {
        if (!g_sessions[i].in_use)
            continue;
        asp_client_closed(g_sessions[i].sess_ref);
        g_sessions[i].in_use = false;
    }
}

// Note traffic from a client so its session does not time out.
static void asp_session_touch(uint8_t session_id) {
    for (int i = 0; i < MAX_ASP_SESS; i++)
        if (g_sessions[i].in_use && (g_sessions[i].sess_ref & 0xFF) == session_id)
            g_sessions[i].last_activity_ns = asp_now_ns();
}

// Close every session that has gone quiet past the ASP timeout, returning
// their forks, locks and desktop references.
static void asp_expire_sessions(void) {
    uint64_t now = asp_now_ns();
    for (int i = 0; i < MAX_ASP_SESS; i++) {
        if (!g_sessions[i].in_use)
            continue;
        if (now < g_sessions[i].last_activity_ns + ASP_SESSION_TIMEOUT_NS)
            continue;
        LOG(1, "ASP: session 0x%04X expired after %llu s of silence", g_sessions[i].sess_ref,
            (unsigned long long)((now - g_sessions[i].last_activity_ns) / 1000000000ULL));
        asp_client_closed(g_sessions[i].sess_ref);
        g_sessions[i].in_use = false;
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

// Look up session by the 1-byte session ID from ATP UserBytes[1]
static asp_session_t *get_session_by_id(uint8_t session_id) {
    for (int i = 0; i < MAX_ASP_SESS; i++)
        if (g_sessions[i].in_use && (g_sessions[i].sess_ref & 0xFF) == session_id)
            return &g_sessions[i];
    return NULL;
}

// Build an ASP reply payload (to be wrapped inside ATP response)
static int asp_build_reply(uint8_t *out, int out_max, uint8_t func, uint16_t sess_ref, uint16_t req_ref,
                           uint16_t result, const uint8_t *data, int data_len) {
    (void)func; // SPFunction travels in ATP UserBytes in this stack
    if (out_max < 6 + data_len)
        return -1;
    out[0] = (sess_ref >> 8) & 0xFF;
    out[1] = sess_ref & 0xFF;
    out[2] = (req_ref >> 8) & 0xFF;
    out[3] = req_ref & 0xFF;
    out[4] = (result >> 8) & 0xFF;
    out[5] = result & 0xFF;
    if (data_len > 0)
        memcpy(&out[6], data, data_len);
    return 6 + data_len;
}

// ASP SPWrite: pending state for the WriteContinue two-transaction flow.
// The factor of 8 matches the ATP burst size cap (which incidentally also
// matches PAP_MAX_FLOW_QUANTUM); both ASP and PAP run over ATP with the
// same 8-fragment burst, but they're independent protocol limits.
#define ASP_WRITE_QUANTUM (8 * ATP_MAX_ATP_PAYLOAD) // 4624 bytes max per WriteContinue

typedef struct {
    bool in_use;
    // Saved original Write transaction context (for deferred response)
    ddp_header_t orig_ddp;
    atp_packet_t orig_atp;
    uint8_t orig_atp_data[ATP_MAX_ATP_PAYLOAD]; // deep copy of ephemeral ATP data
    // AFP command (opcode separated, params follow)
    uint8_t afp_opcode;
    uint8_t afp_params[ATP_MAX_ATP_PAYLOAD];
    int afp_params_len;
    // Write data accumulated from WriteContinue response
    uint8_t write_buf[ASP_WRITE_QUANTUM];
    uint16_t session_ref; // the session that issued the Write
    int write_len;
} asp_pending_write_t;

static asp_pending_write_t g_pending_write;

// Forget every session and the pending write (the machine is going away, or
// coming up).  The client is not told: it drops its own session state.
static void asp_reset(void) {
    memset(g_sessions, 0, sizeof(g_sessions));
    memset(&g_pending_write, 0, sizeof(g_pending_write));
}

// Called for each ATP response fragment from the client's WriteContinueReply
static void asp_wc_on_response(const atp_response_fragment_t *fragment, void *ctx) {
    asp_pending_write_t *pw = (asp_pending_write_t *)ctx;
    if (!pw || !pw->in_use || !fragment || !fragment->data)
        return;
    int avail = ASP_WRITE_QUANTUM - pw->write_len;
    int copy = (fragment->data_len < avail) ? fragment->data_len : avail;
    if (copy > 0) {
        memcpy(pw->write_buf + pw->write_len, fragment->data, (size_t)copy);
        pw->write_len += copy;
    }
}

// Called when WriteContinue transaction completes: process write and reply
static void asp_wc_on_complete(atp_request_handle_t *handle, atp_request_result_t result, void *ctx) {
    (void)handle;
    asp_pending_write_t *pw = (asp_pending_write_t *)ctx;
    if (!pw || !pw->in_use) {
        return;
    }

    uint32_t afp_result;
    uint8_t afp_out[ATP_MAX_ATP_PAYLOAD];
    int afp_len = 0;

    if (result != ATP_REQUEST_RESULT_OK || pw->write_len <= 0) {
        // WriteContinue failed or no data received
        LOG(2, "ASP WriteContinue failed: result=%d write_len=%d", (int)result, pw->write_len);
        afp_result = 0xFFFFEC6Au; // MiscErr
    } else {
        // Build combined buffer: AFP params (11 bytes for FPWrite) + write data
        int combined_len = pw->afp_params_len + pw->write_len;
        uint8_t *combined = (uint8_t *)malloc((size_t)combined_len);
        if (!combined) {
            afp_result = 0xFFFFEC6Au; // MiscErr
        } else {
            memcpy(combined, pw->afp_params, (size_t)pw->afp_params_len);
            memcpy(combined + pw->afp_params_len, pw->write_buf, (size_t)pw->write_len);
            afp_result = asp_client_command(pw->session_ref, pw->afp_opcode, combined, combined_len, afp_out,
                                            (int)sizeof(afp_out), &afp_len);
            free(combined);
        }
    }

    if (afp_result != 0x00000000u)
        afp_len = 0;

    LOG(6, "ASP Write complete: opcode=0x%02X result=0x%08X replyLen=%d dataLen=%d", pw->afp_opcode, afp_result,
        afp_len, pw->write_len);

    // Send deferred response to the original Write ATP transaction
    uint8_t reply_user[4];
    reply_user[0] = (uint8_t)((afp_result >> 24) & 0xFF);
    reply_user[1] = (uint8_t)((afp_result >> 16) & 0xFF);
    reply_user[2] = (uint8_t)((afp_result >> 8) & 0xFF);
    reply_user[3] = (uint8_t)(afp_result & 0xFF);
    atp_responder_send_simple(&pw->orig_ddp, &pw->orig_atp, reply_user, (afp_len > 0) ? afp_out : NULL, afp_len, false);

    pw->in_use = false;
}

static const atp_request_callbacks_t g_asp_wc_callbacks = {
    .on_response = asp_wc_on_response,
    .on_complete = asp_wc_on_complete,
};

// ASP handler: builds replies via ATP responder helpers
static void asp_in(const ddp_header_t *ddp, atp_packet_t *atp, void *ctx) {
    (void)ctx;
    if (!ddp || !atp)
        return;

    uint8_t reply_user[4];
    memcpy(reply_user, atp->user, sizeof(reply_user));

    // SLS GetStatus: no ATP data, SPFunction in UserByte0
    if (atp->data_len == 0 && atp->user[0] == ASP_GET_STAT) {
        LOG(3, "ASP GetStatus: request from node=%u socket=%u", ddp->llap.src, ddp->src_socket);
        uint8_t *status_block = NULL;
        size_t status_len = 0;
        int sb_rc =
            (g_client && g_client->get_status) ? g_client->get_status(g_client_ctx, &status_block, &status_len) : -1;
        if (sb_rc == 0 && status_block && status_len > 0) {
            reply_user[0] = 0; // zero UserBytes for reply
            int payload_len = (status_len > (size_t)ATP_MAX_ATP_PAYLOAD) ? ATP_MAX_ATP_PAYLOAD : (int)status_len;
            atp_responder_send_simple(ddp, atp, reply_user, status_block, payload_len, false);
        } else {
            reply_user[0] = 0;
            reply_user[1] = reply_user[2] = reply_user[3] = 0;
            atp_responder_send_simple(ddp, atp, reply_user, NULL, 0, false);
        }
        if (status_block)
            free(status_block);
        return;
    }

    // OpenSess handshake on SLS: no ATP payload; parameters in UserBytes
    if (atp->data_len == 0 && atp->user[0] == ASP_OPEN_SESS) {
        uint8_t wss = atp->user[1];
        uint16_t asp_ver = ((uint16_t)atp->user[2] << 8) | (uint16_t)atp->user[3];
        (void)wss;
        (void)asp_ver;
        int new_ref = alloc_session();
        uint8_t sss = HOST_AFP_SOCKET;
        uint8_t session_id = 0;
        uint16_t err = 0x0000;
        if (new_ref < 0) {
            LOG(1, "ASP OpenSess: failed (no free sessions) wss=%u ver=0x%04X", wss, asp_ver);
            err = 0x0001;
            sss = 0;
            session_id = 0;
        } else {
            asp_session_t *s = get_session((uint16_t)new_ref);
            if (s) {
                s->wss = wss;
                s->sss = sss;
                s->client_node = ddp->llap.src;
                s->asp_version = asp_ver;
            }
            session_id = (uint8_t)((uint16_t)new_ref & 0xFF);
            LOG(3, "ASP OpenSess: id=0x%02X sss=%u wss=%u ver=0x%04X", session_id, sss, wss, asp_ver);
        }
        reply_user[0] = sss;
        reply_user[1] = session_id;
        reply_user[2] = (uint8_t)((err >> 8) & 0xFF);
        reply_user[3] = (uint8_t)(err & 0xFF);
        atp_responder_send_simple(ddp, atp, reply_user, NULL, 0, false);
        return;
    }

    // ASP Tickle: one-way keepalive (no response).  It is what keeps a
    // session alive; a client that stops tickling is expired by the sweep.
    if (atp->user[0] == ASP_TICKLE) {
        asp_session_touch(atp->user[1]);
        LOG(3, "ASP Tickle: session=0x%02X", atp->user[1]);
        return;
    }

    // Any other traffic is liveness evidence too.
    asp_session_touch(atp->user[1]);

    // Extract ASP fields (lenient); SPFunction carried in UserBytes[0]
    uint8_t func = atp->user[0];
    uint16_t sess_ref = (atp->data_len >= 2) ? (uint16_t)((atp->data[0] << 8) | atp->data[1]) : 0;
    uint16_t req_ref = (atp->data_len >= 4) ? (uint16_t)((atp->data[2] << 8) | atp->data[3]) : 0;
    const uint8_t *adata = NULL;
    int adata_len = 0;
    if (atp->data_len >= 6) {
        adata = &atp->data[6];
        adata_len = atp->data_len - 6;
    } else if (atp->data_len > 4) {
        adata = &atp->data[4];
        adata_len = atp->data_len - 4;
    }

    uint8_t asp_reply[ATP_MAX_ATP_PAYLOAD];
    int asp_reply_len = 0;

    switch (func) {
    case ASP_CLOSE_SESS: {
        LOG(3, "ASP CloseSess: sess_ref=0x%04X req_ref=0x%04X", sess_ref, req_ref);
        if (sess_ref != 0) {
            free_session(sess_ref);
        }
        asp_reply_len = asp_build_reply(asp_reply, sizeof(asp_reply), ASP_CLOSE_SESS, sess_ref, req_ref, 0, NULL, 0);
        break;
    }
    case ASP_COMMAND: {
        const uint8_t *cmd = atp->data;
        int cmd_len = atp->data_len;
        uint8_t opcode = (cmd_len > 0) ? cmd[0] : 0;
        LOG(6, "ASP Command: session=0x%02X seq=0x%04X opcode=0x%02X len=%d", atp->user[1],
            (unsigned)((atp->user[2] << 8) | atp->user[3]), opcode, cmd_len);
        // Count consecutive set bits in bitmap to determine max response packets
        int max_packets = 0;
        for (uint8_t bm = atp->bitmap; bm & 1; bm >>= 1)
            max_packets++;
        if (max_packets < 1)
            max_packets = 1;
        if (max_packets > ATP_MAX_RESPONSE_FRAGMENTS)
            max_packets = ATP_MAX_RESPONSE_FRAGMENTS;
        int out_buf_size = max_packets * ATP_MAX_ATP_PAYLOAD;
        uint8_t afp_out[ATP_MAX_RESPONSE_FRAGMENTS * ATP_MAX_ATP_PAYLOAD];
        int afp_len = 0;
        asp_session_t *cmd_sess = get_session_by_id(atp->user[1]);
        uint32_t afp_result =
            asp_client_command(cmd_sess ? cmd_sess->sess_ref : 0, opcode, (cmd_len > 0) ? (cmd + 1) : NULL,
                               (cmd_len > 0) ? (cmd_len - 1) : 0, afp_out, out_buf_size, &afp_len);
        LOG(6, "ASP Command result: opcode=0x%02X result=0x%08X replyLen=%d", opcode, afp_result, afp_len);
        reply_user[0] = (uint8_t)((afp_result >> 24) & 0xFF);
        reply_user[1] = (uint8_t)((afp_result >> 16) & 0xFF);
        reply_user[2] = (uint8_t)((afp_result >> 8) & 0xFF);
        reply_user[3] = (uint8_t)(afp_result & 0xFF);
        if (afp_len <= ATP_MAX_ATP_PAYLOAD) {
            atp_responder_send_simple(ddp, atp, reply_user, (afp_len > 0) ? afp_out : NULL, afp_len, false);
        } else {
            // Split reply across multiple ATP response packets
            int num_packets = (afp_len + ATP_MAX_ATP_PAYLOAD - 1) / ATP_MAX_ATP_PAYLOAD;
            if (num_packets > max_packets)
                num_packets = max_packets;
            atp_response_packet_desc_t descs[ATP_MAX_RESPONSE_FRAGMENTS];
            int offset = 0;
            for (int i = 0; i < num_packets; i++) {
                int chunk = afp_len - offset;
                if (chunk > ATP_MAX_ATP_PAYLOAD)
                    chunk = ATP_MAX_ATP_PAYLOAD;
                descs[i].payload = afp_out + offset;
                descs[i].payload_len = chunk;
                descs[i].user = reply_user;
                descs[i].sts = false;
                descs[i].eom = (i == num_packets - 1);
                offset += chunk;
            }
            atp_responder_send_packets(ddp, atp, descs, (size_t)num_packets);
        }
        return;
    }
    case ASP_GET_STAT: {
        uint8_t opcode = (adata_len > 0) ? adata[0] : 0;
        uint8_t afp_out[ATP_MAX_ATP_PAYLOAD];
        int afp_len = 0;
        uint32_t afp_result =
            asp_client_command(0, opcode, (adata_len > 0) ? (adata + 1) : NULL, (adata_len > 0) ? (adata_len - 1) : 0,
                               afp_out, sizeof(afp_out), &afp_len);
        uint16_t asp_result = (uint16_t)(afp_result & 0xFFFFu);
        if (afp_result != 0x00000000u) {
            afp_len = 0;
        }
        asp_reply_len = asp_build_reply(asp_reply, sizeof(asp_reply), ASP_GET_STAT, sess_ref, req_ref, asp_result,
                                        (afp_len > 0 && afp_result == 0x00000000u) ? afp_out : NULL,
                                        (afp_result == 0x00000000u) ? afp_len : 0);
        break;
    }
    case ASP_WRITE: {
        // ASP SPWrite: two-transaction protocol via WriteContinue
        const uint8_t *cmd = atp->data;
        int cmd_len = atp->data_len;
        uint8_t opcode = (cmd_len > 0) ? cmd[0] : 0;
        uint8_t session_id = atp->user[1];
        LOG(6, "ASP Write: session=0x%02X seq=0x%04X opcode=0x%02X len=%d", session_id,
            (unsigned)((atp->user[2] << 8) | atp->user[3]), opcode, cmd_len);

        // Look up session for client addressing
        asp_session_t *sess = get_session_by_id(session_id);
        if (!sess) {
            LOG(2, "ASP Write: unknown session 0x%02X", session_id);
            uint32_t err = 0xFFFFEC62u; // SessClosed
            reply_user[0] = (uint8_t)((err >> 24) & 0xFF);
            reply_user[1] = (uint8_t)((err >> 16) & 0xFF);
            reply_user[2] = (uint8_t)((err >> 8) & 0xFF);
            reply_user[3] = (uint8_t)(err & 0xFF);
            atp_responder_send_simple(ddp, atp, reply_user, NULL, 0, false);
            return;
        }

        if (g_pending_write.in_use) {
            // Already processing a write; ignore duplicate (XO handles retransmit)
            LOG(6, "ASP Write: already pending, ignoring");
            return;
        }

        // Save context for deferred response
        asp_pending_write_t *pw = &g_pending_write;
        memset(pw, 0, sizeof(*pw));
        pw->in_use = true;
        pw->orig_ddp = *ddp;
        pw->orig_atp = *atp;
        // Deep-copy ATP data (ephemeral pointer into receive buffer)
        if (atp->data && atp->data_len > 0) {
            int dlen =
                (atp->data_len > (int)sizeof(pw->orig_atp_data)) ? (int)sizeof(pw->orig_atp_data) : atp->data_len;
            memcpy(pw->orig_atp_data, atp->data, (size_t)dlen);
            pw->orig_atp.data = pw->orig_atp_data;
            pw->orig_atp.data_len = dlen;
        }
        pw->session_ref = sess->sess_ref;
        pw->afp_opcode = opcode;
        if (cmd_len > 1) {
            pw->afp_params_len = cmd_len - 1;
            if (pw->afp_params_len > (int)sizeof(pw->afp_params))
                pw->afp_params_len = (int)sizeof(pw->afp_params);
            memcpy(pw->afp_params, cmd + 1, (size_t)pw->afp_params_len);
        }

        // Send WriteContinue ATP request to the client's WSS
        uint16_t buf_size = ASP_WRITE_QUANTUM;
        uint8_t wc_data[2];
        wc_data[0] = (uint8_t)((buf_size >> 8) & 0xFF);
        wc_data[1] = (uint8_t)(buf_size & 0xFF);

        atp_request_params_t wc_params = {
            .dest = {.net = 0, .node = sess->client_node, .socket = sess->wss},
            .src_socket = HOST_AFP_SOCKET,
            .bitmap = 0xFF, // request up to 8 response packets
            .mode = ATP_TRANSACTION_XO,
            .trel_timer_hint = 0,
            .user = {ASP_WRITE_CONTINUE, session_id, atp->user[2], atp->user[3]},
            .payload = wc_data,
            .payload_len = 2,
            .retry_timeout_ms = 5000,
            .retry_limit = 10,
        };

        LOG(6, "ASP WriteContinue: node=%u wss=%u bufSize=%u", sess->client_node, sess->wss, buf_size);
        atp_request_handle_t *wc_handle = atp_request_submit(&wc_params, &g_asp_wc_callbacks, pw);
        if (!wc_handle) {
            LOG(2, "ASP WriteContinue: failed to submit ATP request");
            pw->in_use = false;
            uint32_t err = 0xFFFFEC6Au; // MiscErr
            reply_user[0] = (uint8_t)((err >> 24) & 0xFF);
            reply_user[1] = (uint8_t)((err >> 16) & 0xFF);
            reply_user[2] = (uint8_t)((err >> 8) & 0xFF);
            reply_user[3] = (uint8_t)(err & 0xFF);
            atp_responder_send_simple(ddp, atp, reply_user, NULL, 0, false);
        }
        return; // response deferred until WriteContinue completes
    }
    default: {
        LOG(3, "ASP unknown func=0x%02X sess=0x%04X req=0x%04X dataLen=%d", func, sess_ref, req_ref, atp->data_len);
        asp_reply_len =
            asp_build_reply(asp_reply, sizeof(asp_reply), (func == 0xFF) ? 0 : func, sess_ref, req_ref, 0, NULL, 0);
        break;
    }
    }

    if (asp_reply_len < 0)
        asp_reply_len = 0;
    atp_responder_send_simple(ddp, atp, reply_user, asp_reply_len > 0 ? asp_reply : NULL, asp_reply_len, false);
}

// === Lifecycle ==================================================================

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
