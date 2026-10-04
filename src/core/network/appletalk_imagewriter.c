// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_imagewriter.c
// The ImageWriter II's LocalTalk Option card: a PAP server for the machine's
// virtual ImageWriter (iw_printer.h) while it is connected to LocalTalk.
//
// The card registers `<name>:ImageWriter@*` on NBP, accepts one connection
// at a time, pulls the job with SendData and feeds the data -- the same
// ImageWriter byte stream a serial port carries -- to the printer; the
// workstation's EOF ends the job.  It answers SendStatus and OpenConn with a
// binary statusBits word where the LaserWriter sends a status string (Apple
// Technical Note NW20, "PAP Status Buffer").  Nothing here interprets
// PostScript or answers queries, so it is a separate, small server beside
// the LaserWriter's (appletalk_printer.c), on its own socket: both printers
// can be on the network at once, as in an office.
//
// Sources: Inside AppleTalk, 2nd ed., ch. 10 (PAP); Technical Note NW20.

#include "appletalk.h"
#include "appletalk_internal.h"
#include "iw_printer.h"
#include "log.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("pap");

#define IW_PAP_OBJECT_DEFAULT "Virtual ImageWriter"
#define IW_PAP_OBJECT_MAX     32
#define IW_PAP_TYPE           "ImageWriter"
// Guest time between a transaction's end and the next SendData (as the
// LaserWriter's server spaces its reads, appletalk_printer.c)
#define IW_PAP_GAP_NS      10000000ull
#define IW_PAP_RETRY_MS    8000u
#define IW_PAP_RETRY_LIMIT 12
#define IW_PAP_TIMEOUT_NS  (120ull * 1000000000ull)
// The workstation's read credits held until the job ends
#define IW_PAP_MAX_CREDITS 8

// statusBits (NW20): sheet feeder installed, paper out, paper jam (what a
// printer with a sheet feeder reports for running out of paper)
#define IW_STATUS_SHEET_FEEDER (1u << 14)
#define IW_STATUS_PAPER_OUT    (1u << 13)
#define IW_STATUS_PAPER_JAM    (1u << 10)

// A held SendData from the workstation (its read of our output)
typedef struct {
    ddp_header_t ddp;
    atp_packet_t atp;
} iw_credit_t;

// The connection (one at a time)
typedef struct {
    bool active;
    uint8_t conn_id;
    atalk_socket_addr_t client;
    uint8_t quantum; // packets per SendData
    uint16_t next_seq;
    bool awaiting; // a SendData is outstanding
    bool eof; // the transaction in flight carried the EOF
    atp_request_handle_t *send;
    uint64_t last_activity_ns;
    iw_credit_t credits[IW_PAP_MAX_CREDITS];
    uint8_t n_credits;
} iw_session_t;

static struct {
    bool registered; // the socket handler is installed
    bool published; // the NBP entry is up
    char object[IW_PAP_OBJECT_MAX + 1];
    atalk_nbp_entry_t *nbp;
} g_iw = {.object = IW_PAP_OBJECT_DEFAULT};

static iw_session_t g_s;
static atalk_timer_t g_gap_timer;
static atalk_timer_t g_idle_timer;

static void iw_pap_close(bool notify);

// The status word the printer would report now
static uint16_t status_bits(const iw_printer_t *p) {
    uint16_t bits = 0;
    if (!p)
        return 0;
    bool feeder = iw_printer_sheet_feeder(p);
    if (feeder)
        bits |= IW_STATUS_SHEET_FEEDER;
    if (iw_printer_paper_out(p))
        bits |= feeder ? IW_STATUS_PAPER_JAM : IW_STATUS_PAPER_OUT;
    return bits;
}

// The ATP data of an OpenConnReply / Status: socket, flow quantum, result,
// then from offset 4 the status data -- a length byte of 2 and the
// statusBits word, big-endian.  (The layout the AppleTalk ImageWriter
// driver reads: with bit 13 set it reports the printer out of paper.)
static int status_payload(uint16_t result, uint8_t out[7]) {
    uint16_t bits = status_bits(iw_printer_localtalk());
    out[0] = HOST_IW_PAP_SOCKET;
    out[1] = PAP_MAX_FLOW_QUANTUM;
    out[2] = (uint8_t)(result >> 8);
    out[3] = (uint8_t)result;
    out[4] = 2;
    out[5] = (uint8_t)(bits >> 8);
    out[6] = (uint8_t)bits;
    return 7;
}

// Answer every held read with an empty EOF: the job is over.
static void release_credits(void) {
    for (int i = 0; i < g_s.n_credits; i++) {
        uint8_t user[4] = {g_s.conn_id, PAP_FUNC_DATA, 1, 0};
        atp_response_packet_desc_t desc = {.payload = NULL, .payload_len = 0, .user = user, .sts = false, .eom = true};
        atp_responder_send_packets(&g_s.credits[i].ddp, &g_s.credits[i].atp, &desc, 1);
    }
    g_s.n_credits = 0;
}

static void session_reset(void) {
    atalk_timer_cancel_all(&g_gap_timer);
    atalk_timer_cancel_all(&g_idle_timer);
    if (g_s.send)
        atp_request_cancel(g_s.send);
    memset(&g_s, 0, sizeof(g_s));
}

static void on_fragment(const atp_response_fragment_t *f, void *ctx);
static void on_complete(atp_request_handle_t *h, atp_request_result_t result, void *ctx);

// Pull the next flow quantum of data from the workstation.
static void issue_senddata(void) {
    if (!g_s.active || g_s.awaiting)
        return;
    // The printer must take data: selected, with paper; otherwise wait
    iw_printer_t *p = iw_printer_localtalk();
    if (!p || !iw_printer_selected(p) || iw_printer_paper_out(p)) {
        atalk_timer_arm(&g_gap_timer, 0, IW_PAP_GAP_NS * 10);
        return;
    }
    uint8_t q = g_s.quantum ? g_s.quantum : PAP_MAX_FLOW_QUANTUM;
    uint16_t seq = g_s.next_seq++;
    if (g_s.next_seq == 0)
        g_s.next_seq = 1;
    atp_request_params_t params = {.dest = g_s.client,
                                   .src_socket = HOST_IW_PAP_SOCKET,
                                   .bitmap = (uint8_t)(q >= 8 ? 0xFF : (1u << q) - 1u),
                                   .mode = ATP_TRANSACTION_XO,
                                   .trel_timer_hint = 2,
                                   .retry_timeout_ms = IW_PAP_RETRY_MS,
                                   .retry_limit = IW_PAP_RETRY_LIMIT};
    params.user[0] = g_s.conn_id;
    params.user[1] = PAP_FUNC_SENDDATA;
    params.user[2] = (uint8_t)(seq >> 8);
    params.user[3] = (uint8_t)seq;
    atp_request_callbacks_t cb = {.on_response = on_fragment, .on_complete = on_complete};
    g_s.send = atp_request_submit(&params, &cb, &g_s);
    if (!g_s.send) {
        LOG(1, "imagewriter pap: SendData could not be submitted");
        iw_pap_close(true);
        return;
    }
    g_s.awaiting = true;
    g_s.last_activity_ns = atalk_now_ns();
}

static void gap_cb(void *source, uint64_t data) {
    (void)source;
    (void)data;
    issue_senddata();
}

// The connection timer: a workstation silent for two minutes is gone.
static void idle_cb(void *source, uint64_t data) {
    (void)source;
    (void)data;
    if (!g_s.active)
        return;
    uint64_t idle = atalk_now_ns() - g_s.last_activity_ns;
    if (idle >= IW_PAP_TIMEOUT_NS) {
        LOG(1, "imagewriter pap: connection timeout");
        iw_pap_close(true);
        return;
    }
    atalk_timer_arm(&g_idle_timer, 0, IW_PAP_TIMEOUT_NS - idle);
}

// One Data packet: its bytes go to the printer as they come.
static void on_fragment(const atp_response_fragment_t *f, void *ctx) {
    (void)ctx;
    if (!g_s.active || !f || f->duplicate)
        return;
    g_s.last_activity_ns = atalk_now_ns();
    iw_printer_t *p = iw_printer_localtalk();
    if (p && f->data && f->data_len > 0)
        iw_printer_feed(p, f->data, (size_t)f->data_len);
    if (f->user[2])
        g_s.eof = true;
}

// A SendData transaction ended: the EOF ends the job, else read on.
static void on_complete(atp_request_handle_t *h, atp_request_result_t result, void *ctx) {
    (void)ctx;
    if (g_s.send == h)
        g_s.send = NULL;
    g_s.awaiting = false;
    if (!g_s.active)
        return;
    if (result == ATP_REQUEST_RESULT_TIMEOUT) {
        LOG(1, "imagewriter pap: SendData timed out");
        iw_pap_close(true);
        return;
    }
    if (result != ATP_REQUEST_RESULT_OK)
        return;
    if (g_s.eof) {
        g_s.eof = false;
        LOG(2, "imagewriter pap: EOF, job complete");
        iw_printer_end_job(iw_printer_localtalk());
        release_credits();
        // The workstation closes the connection, or sends the next job
    }
    atalk_timer_arm(&g_gap_timer, 0, IW_PAP_GAP_NS);
}

// End the connection (optionally telling the workstation); a job cut off
// ends with what arrived.
static void iw_pap_close(bool notify) {
    if (!g_s.active) {
        session_reset();
        return;
    }
    if (notify && g_s.client.socket) {
        atp_request_params_t params = {.dest = g_s.client,
                                       .src_socket = HOST_IW_PAP_SOCKET,
                                       .bitmap = 1,
                                       .mode = ATP_TRANSACTION_XO,
                                       .retry_timeout_ms = 2000,
                                       .retry_limit = 2};
        params.user[0] = g_s.conn_id;
        params.user[1] = PAP_FUNC_CLOSE;
        atp_request_callbacks_t cb = {0};
        atp_request_submit(&params, &cb, NULL);
    }
    release_credits();
    iw_printer_t *p = iw_printer_localtalk();
    if (p && iw_printer_busy(p))
        iw_printer_end_job(p);
    session_reset();
}

static void handle_open(const ddp_header_t *ddp, atp_packet_t *atp) {
    uint8_t conn_id = atp->user[0];
    uint16_t result = PAP_RESULT_OK;
    iw_printer_t *p = iw_printer_localtalk();
    if (!p || !g_iw.published || g_s.active || iw_printer_busy(p) || atp->data_len < 2) {
        result = PAP_RESULT_BUSY;
    } else {
        session_reset();
        g_s.active = true;
        g_s.conn_id = conn_id;
        g_s.client.net = ddp->src_net;
        g_s.client.node = ddp->llap.src;
        g_s.client.socket = atp->data[0];
        g_s.quantum = PAP_MAX_FLOW_QUANTUM;
        g_s.next_seq = 1;
        g_s.last_activity_ns = atalk_now_ns();
        atalk_timer_arm(&g_idle_timer, 0, IW_PAP_TIMEOUT_NS);
    }
    uint8_t payload[7];
    int len = status_payload(result, payload);
    uint8_t user[4] = {conn_id, PAP_FUNC_OPEN_REPLY, 0, 0};
    LOG(2, "imagewriter pap: OpenConn conn=%u -> %s", (unsigned)conn_id, result == PAP_RESULT_OK ? "ok" : "busy");
    atp_responder_send_simple(ddp, atp, user, payload, len, false);
    if (result == PAP_RESULT_OK)
        atalk_timer_arm(&g_gap_timer, 0, IW_PAP_GAP_NS);
}

static void handle_status(const ddp_header_t *ddp, atp_packet_t *atp) {
    uint8_t payload[7];
    int len = status_payload(PAP_RESULT_OK, payload);
    payload[0] = payload[1] = payload[2] = payload[3] = 0; // unused in a Status (NW20)
    uint8_t user[4] = {0, PAP_FUNC_STATUS, 0, 0};
    atp_responder_send_simple(ddp, atp, user, payload, len, false);
}

static bool is_session(const ddp_header_t *ddp, const atp_packet_t *atp) {
    return g_s.active && atp->user[0] == g_s.conn_id && ddp->llap.src == g_s.client.node &&
           ddp->src_net == g_s.client.net;
}

static void handle_request(const ddp_header_t *ddp, atp_packet_t *atp, void *ctx) {
    (void)ctx;
    if (!ddp || !atp)
        return;
    switch (atp->user[1]) {
    case PAP_FUNC_OPEN:
        handle_open(ddp, atp);
        break;
    case PAP_FUNC_SEND_STATUS:
        handle_status(ddp, atp);
        break;
    case PAP_FUNC_TICKLE:
        if (is_session(ddp, atp))
            g_s.last_activity_ns = atalk_now_ns();
        break;
    case PAP_FUNC_CLOSE: {
        uint8_t user[4] = {atp->user[0], PAP_FUNC_CLOSE_REPLY, 0, 0};
        atp_responder_send_simple(ddp, atp, user, NULL, 0, false);
        if (is_session(ddp, atp)) {
            LOG(2, "imagewriter pap: closed by the workstation");
            iw_pap_close(false);
        }
        break;
    }
    case PAP_FUNC_SENDDATA:
        // The workstation reads our output: the card sends none, so the read
        // is held and answered with EOF when the job ends
        if (is_session(ddp, atp) && g_s.n_credits < IW_PAP_MAX_CREDITS) {
            g_s.credits[g_s.n_credits].ddp = *ddp;
            g_s.credits[g_s.n_credits].atp = *atp;
            g_s.n_credits++;
        } else {
            uint8_t user[4] = {atp->user[0], PAP_FUNC_DATA, 1, 0};
            atp_responder_send_simple(ddp, atp, user, NULL, 0, false);
        }
        break;
    default:
        break;
    }
}

void atalk_imagewriter_register(void) {
    if (g_iw.registered)
        return;
    static const atp_socket_handler_t handler = {.handle_request = handle_request};
    atp_register_socket_handler(HOST_IW_PAP_SOCKET, &handler, NULL);
    g_iw.registered = true;
}

void atalk_imagewriter_register_timers(struct atalk_conn *conn) {
    atalk_timer_init(conn, &g_gap_timer, "iwpap", "senddata_gap", &gap_cb);
    atalk_timer_init(conn, &g_idle_timer, "iwpap", "idle", &idle_cb);
}

void atalk_imagewriter_unplug(void) {
    // The workstation is gone; so is its connection.  The printer (a part of
    // the machine) ends the job with what arrived.
    session_reset();
}

int atalk_imagewriter_publish(bool on) {
    if (on == g_iw.published)
        return 0;
    if (!on) {
        iw_pap_close(true);
        atalk_nbp_withdraw(&g_iw.nbp);
        g_iw.published = false;
        LOG(2, "imagewriter pap: withdrawn");
        return 0;
    }
    if (!appletalk_network())
        return -1;
    atalk_imagewriter_register();
    atalk_nbp_service_desc_t desc = {.object = g_iw.object,
                                     .type = IW_PAP_TYPE,
                                     .zone = "*",
                                     .socket = HOST_IW_PAP_SOCKET,
                                     .node = LLAP_HOST_NODE,
                                     .net = 0};
    if (atalk_nbp_publish(&g_iw.nbp, &desc) != 0) {
        LOG(1, "imagewriter pap: cannot publish '%s'", g_iw.object);
        return -1;
    }
    g_iw.published = true;
    LOG(2, "imagewriter pap: published as '%s:%s@*'", g_iw.object, IW_PAP_TYPE);
    return 0;
}

bool atalk_imagewriter_published(void) {
    return g_iw.published;
}

const char *atalk_imagewriter_name(void) {
    return g_iw.object;
}

int atalk_imagewriter_set_name(const char *name) {
    if (!name || !*name || strlen(name) > IW_PAP_OBJECT_MAX)
        return -1;
    char old[IW_PAP_OBJECT_MAX + 1];
    memcpy(old, g_iw.object, sizeof(old));
    snprintf(g_iw.object, sizeof(g_iw.object), "%s", name);
    if (!g_iw.published)
        return 0;
    // Re-publish under the new name; on failure the old one stays
    atalk_nbp_service_desc_t desc = {.object = g_iw.object,
                                     .type = IW_PAP_TYPE,
                                     .zone = "*",
                                     .socket = HOST_IW_PAP_SOCKET,
                                     .node = LLAP_HOST_NODE,
                                     .net = 0};
    if (atalk_nbp_publish(&g_iw.nbp, &desc) != 0) {
        memcpy(g_iw.object, old, sizeof(old));
        return -1;
    }
    return 0;
}
