// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Protocol unit tests for the ADSP endpoint (src/core/network/appletalk_adsp.c).
//
// Two engine instances run back to back over an in-process loopback that
// stands in for DDP: every packet one endpoint emits is handed to the other,
// optionally after a deterministic drop filter.  No guest, no emulator, no
// wall clock — the suite owns the clock, so timer behaviour (open retries,
// the connection timer, data retransmission) is exercised exactly.
//
// Wire expectations are transcribed from Inside AppleTalk, 2nd ed., ch. 12
// (packet format 12-12, control codes 12-14, data-flow examples 12-15,
// attention 12-19, open dialog 12-22 ff., closing 12-38), distilled in
// docs/core/network/appletalk.md §III.3.

#include "appletalk_adsp.h"
#include "test_assert.h"

#include <stdio.h>
#include <string.h>

#define NODE_A 1
#define NODE_B 2
#define SOCK_A 20
#define SOCK_B 10

// ============================================================================
// Loopback wire
// ============================================================================

typedef struct {
    uint8_t from_node;
    atalk_socket_addr_t dest;
    uint8_t src_socket;
    int len;
    uint8_t buf[ADSP_HEADER_SIZE + ADSP_MAX_DATA];
} wire_pkt_t;

#define WIRE_MAX 512

static wire_pkt_t g_wire[WIRE_MAX];
static int g_wire_head;
static int g_wire_tail;
static uint64_t g_now_ns;

static adsp_stack_t *g_a;
static adsp_stack_t *g_b;

// Deterministic loss: drop the first `g_drop_data` data packets we see.
static int g_drop_data;
static int g_dropped;
static bool g_partition; // drop everything (models an unreachable peer)

// What each endpoint's client saw.
typedef struct {
    int opens;
    int closes;
    int accepts;
    bool refuse; // make on_accept deny
    char last_close[128];
    int data_events;
    int eom_events;
    int attn_events;
    uint16_t attn_code;
    char attn_payload[64];
    int stream_len;
    uint8_t stream[8192];
    adsp_conn_t *last_conn;
} recorder_t;

static recorder_t g_rec_a;
static recorder_t g_rec_b;

static uint64_t clock_now(void *ctx) {
    (void)ctx;
    return g_now_ns;
}

static int wire_send(void *ctx, const atalk_socket_addr_t *dest, uint8_t src_socket, const uint8_t *pkt, int len) {
    ASSERT_TRUE(g_wire_tail < WIRE_MAX);
    wire_pkt_t *w = &g_wire[g_wire_tail++];
    w->from_node = (uint8_t)(uintptr_t)ctx;
    w->dest = *dest;
    w->src_socket = src_socket;
    w->len = len;
    memcpy(w->buf, pkt, (size_t)len);
    return 0;
}

// True if the packet carries stream data (neither control nor attention).
static bool pkt_is_data(const wire_pkt_t *w) {
    if (w->len < ADSP_HEADER_SIZE)
        return false;
    uint8_t desc = w->buf[12];
    return !(desc & ADSP_DESC_CONTROL) && !(desc & ADSP_DESC_ATTENTION);
}

// Deliver everything queued, letting the responses queue up behind it, until
// the wire goes quiet.  The iteration cap turns a protocol loop into a test
// failure rather than a hang.
static void pump(void) {
    int guard = 0;
    while (g_wire_head < g_wire_tail) {
        ASSERT_TRUE(++guard < 4096);
        wire_pkt_t w = g_wire[g_wire_head++];
        if (g_partition)
            continue;
        if (g_drop_data > 0 && pkt_is_data(&w)) {
            g_drop_data--;
            g_dropped++;
            continue;
        }
        atalk_socket_addr_t from = {.net = 0, .node = w.from_node, .socket = w.src_socket};
        adsp_stack_t *target = (w.dest.node == NODE_A) ? g_a : g_b;
        adsp_input(target, &from, w.dest.socket, w.buf, w.len);
    }
    g_wire_head = 0;
    g_wire_tail = 0;
}

static int wire_pending(void) {
    return g_wire_tail - g_wire_head;
}

// ============================================================================
// Recording client
// ============================================================================

static bool rec_accept(void *ctx, const atalk_socket_addr_t *from) {
    (void)from;
    recorder_t *r = (recorder_t *)ctx;
    r->accepts++;
    return !r->refuse;
}
static void rec_open(void *ctx, adsp_conn_t *c) {
    recorder_t *r = (recorder_t *)ctx;
    r->opens++;
    r->last_conn = c;
}
static void rec_data(void *ctx, adsp_conn_t *c, const uint8_t *data, int len, bool eom) {
    (void)c;
    recorder_t *r = (recorder_t *)ctx;
    r->data_events++;
    if (eom)
        r->eom_events++;
    ASSERT_TRUE(r->stream_len + len <= (int)sizeof(r->stream));
    memcpy(r->stream + r->stream_len, data, (size_t)len);
    r->stream_len += len;
}
static void rec_attn(void *ctx, adsp_conn_t *c, uint16_t code, const uint8_t *data, int len) {
    (void)c;
    recorder_t *r = (recorder_t *)ctx;
    r->attn_events++;
    r->attn_code = code;
    int n = len < (int)sizeof(r->attn_payload) - 1 ? len : (int)sizeof(r->attn_payload) - 1;
    memcpy(r->attn_payload, data, (size_t)n);
    r->attn_payload[n] = '\0';
}
static void rec_close(void *ctx, adsp_conn_t *c, const char *reason) {
    (void)c;
    recorder_t *r = (recorder_t *)ctx;
    r->closes++;
    snprintf(r->last_close, sizeof(r->last_close), "%s", reason ? reason : "");
}

static const adsp_client_t g_client = {
    .on_accept = rec_accept,
    .on_open = rec_open,
    .on_data = rec_data,
    .on_attention = rec_attn,
    .on_close = rec_close,
};

// ============================================================================
// Fixture
// ============================================================================

// A fresh pair of endpoints with B listening on SOCK_B.
static void setup(void) {
    if (g_a)
        adsp_stack_free(g_a);
    if (g_b)
        adsp_stack_free(g_b);
    memset(&g_rec_a, 0, sizeof(g_rec_a));
    memset(&g_rec_b, 0, sizeof(g_rec_b));
    g_wire_head = g_wire_tail = 0;
    g_now_ns = 0;
    g_drop_data = 0;
    g_dropped = 0;
    g_partition = false;

    adsp_config_t ca = {.ctx = (void *)(uintptr_t)NODE_A, .now_ns = clock_now, .send = wire_send};
    adsp_config_t cb = {.ctx = (void *)(uintptr_t)NODE_B, .now_ns = clock_now, .send = wire_send};
    g_a = adsp_stack_new(&ca);
    g_b = adsp_stack_new(&cb);
    ASSERT_TRUE(g_a && g_b);
    ASSERT_EQ_INT(adsp_listen(g_b, SOCK_B, &g_client, &g_rec_b), 0);
}

static adsp_conn_t *open_a_to_b(void) {
    atalk_socket_addr_t dest = {.net = 0, .node = NODE_B, .socket = SOCK_B};
    adsp_conn_t *c = adsp_open(g_a, &dest, SOCK_A, &g_client, &g_rec_a);
    ASSERT_TRUE(c != NULL);
    pump();
    return c;
}

// First live connection on a stack, or NULL.
static adsp_conn_t *first_conn(adsp_stack_t *s) {
    for (int i = 0; i < adsp_conn_slot_max(s); i++) {
        adsp_conn_t *c = adsp_conn_at(s, i);
        if (c)
            return c;
    }
    return NULL;
}

static int conn_count(adsp_stack_t *s) {
    int n = 0;
    for (int i = 0; i < adsp_conn_slot_max(s); i++)
        if (adsp_conn_at(s, i))
            n++;
    return n;
}

static void advance(uint64_t ns) {
    g_now_ns += ns;
    adsp_run_timers(g_a);
    adsp_run_timers(g_b);
    pump();
}

// ============================================================================
// Tests
// ============================================================================

// The first packet of the dialog, byte for byte: descriptor $81, our ConnID
// in the header, version $0100 and a zero destination ConnID in the open
// parameters (Inside AppleTalk 12-27, table of descriptor values).
TEST(test_open_request_wire_format) {
    setup();
    atalk_socket_addr_t dest = {.net = 0, .node = NODE_B, .socket = SOCK_B};
    adsp_conn_t *c = adsp_open(g_a, &dest, SOCK_A, &g_client, &g_rec_a);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ_INT(wire_pending(), 1);

    const wire_pkt_t *w = &g_wire[g_wire_head];
    ASSERT_EQ_INT(w->len, ADSP_HEADER_SIZE + ADSP_OPEN_PARAMS_SIZE);
    ASSERT_EQ_INT(w->dest.node, NODE_B);
    ASSERT_EQ_INT(w->dest.socket, SOCK_B);
    ASSERT_EQ_INT(w->src_socket, SOCK_A);
    ASSERT_EQ_INT(w->buf[12], ADSP_DESC_CONTROL | ADSP_CTL_OPEN_REQ); // $81
    uint16_t src_cid = (uint16_t)((w->buf[0] << 8) | w->buf[1]);
    ASSERT_TRUE(src_cid != 0); // a ConnID of 0 means "unknown" (12-6)
    ASSERT_EQ_INT(src_cid, adsp_conn_local_cid(c));
    // PktFirstByteSeq / PktNextRecvSeq start at 0 (12-7); the window is ours.
    ASSERT_EQ_INT((int)((w->buf[6] << 24) | (w->buf[7] << 16) | (w->buf[8] << 8) | w->buf[9]), 0);
    ASSERT_EQ_INT((int)((w->buf[10] << 8) | w->buf[11]), ADSP_RECV_WINDOW);
    // Open-connection parameters: version, destination ConnID, AttnRecvSeq.
    ASSERT_EQ_INT((int)((w->buf[13] << 8) | w->buf[14]), ADSP_VERSION);
    ASSERT_EQ_INT((int)((w->buf[15] << 8) | w->buf[16]), 0);
    ASSERT_EQ_INT((int)w->buf[20], 0);
    pump();
}

// Figure 12-8: request, request+ack, ack — both ends open, one connection each.
TEST(test_open_dialog) {
    setup();
    adsp_conn_t *ca = open_a_to_b();
    adsp_conn_t *cb = first_conn(g_b);

    ASSERT_TRUE(cb != NULL);
    ASSERT_EQ_INT((int)adsp_conn_state(ca), (int)ADSP_STATE_OPEN);
    ASSERT_EQ_INT((int)adsp_conn_state(cb), (int)ADSP_STATE_OPEN);
    ASSERT_EQ_INT(g_rec_a.opens, 1);
    ASSERT_EQ_INT(g_rec_b.opens, 1);
    ASSERT_EQ_INT(g_rec_b.accepts, 1);
    ASSERT_EQ_INT(conn_count(g_a), 1);
    ASSERT_EQ_INT(conn_count(g_b), 1);
    // The ends learned each other's ConnIDs (12-6).
    ASSERT_EQ_INT(adsp_conn_local_cid(ca), adsp_conn_remote_cid(cb));
    ASSERT_EQ_INT(adsp_conn_local_cid(cb), adsp_conn_remote_cid(ca));
    ASSERT_TRUE(adsp_conn_initiator(ca));
    ASSERT_TRUE(!adsp_conn_initiator(cb));
    ASSERT_EQ_INT((int)adsp_get_stats(g_a)->opens, 1);
}

// Figure 12-9: both peers open at once on the same socket pair, and exactly
// one connection results at each end.
TEST(test_simultaneous_open) {
    setup();
    ASSERT_EQ_INT(adsp_listen(g_a, SOCK_A, &g_client, &g_rec_a), 0);
    atalk_socket_addr_t to_b = {.net = 0, .node = NODE_B, .socket = SOCK_B};
    atalk_socket_addr_t to_a = {.net = 0, .node = NODE_A, .socket = SOCK_A};
    adsp_conn_t *ca = adsp_open(g_a, &to_b, SOCK_A, &g_client, &g_rec_a);
    adsp_conn_t *cb = adsp_open(g_b, &to_a, SOCK_B, &g_client, &g_rec_b);
    ASSERT_TRUE(ca && cb);
    pump();

    ASSERT_EQ_INT(conn_count(g_a), 1);
    ASSERT_EQ_INT(conn_count(g_b), 1);
    ASSERT_EQ_INT((int)adsp_conn_state(ca), (int)ADSP_STATE_OPEN);
    ASSERT_EQ_INT((int)adsp_conn_state(cb), (int)ADSP_STATE_OPEN);
    ASSERT_EQ_INT(g_rec_a.opens, 1);
    ASSERT_EQ_INT(g_rec_b.opens, 1);
}

// Figure 12-10: no listener, so the request is denied and the requester's end
// goes away with a readable reason.
TEST(test_open_denied_without_listener) {
    setup();
    atalk_socket_addr_t dest = {.net = 0, .node = NODE_B, .socket = 99};
    adsp_conn_t *c = adsp_open(g_a, &dest, SOCK_A, &g_client, &g_rec_a);
    ASSERT_TRUE(c != NULL);
    pump();

    ASSERT_EQ_INT(conn_count(g_a), 0);
    ASSERT_EQ_INT(conn_count(g_b), 0);
    ASSERT_EQ_INT(g_rec_a.opens, 0);
    ASSERT_EQ_INT(g_rec_a.closes, 1);
    ASSERT_TRUE(strstr(g_rec_a.last_close, "denied") != NULL);
    ASSERT_TRUE(adsp_get_stats(g_b)->open_denials >= 1);
}

// The connection-opening filter of 12-36: the listener's client refuses.
TEST(test_open_refused_by_client) {
    setup();
    g_rec_b.refuse = true;
    atalk_socket_addr_t dest = {.net = 0, .node = NODE_B, .socket = SOCK_B};
    adsp_conn_t *c = adsp_open(g_a, &dest, SOCK_A, &g_client, &g_rec_a);
    ASSERT_TRUE(c != NULL);
    pump();

    ASSERT_EQ_INT(g_rec_b.accepts, 1);
    ASSERT_EQ_INT(conn_count(g_b), 0);
    ASSERT_EQ_INT(conn_count(g_a), 0);
    ASSERT_EQ_INT(g_rec_a.closes, 1);
}

// An incompatible ADSP version must be denied rather than accepted (12-27).
TEST(test_open_version_mismatch_denied) {
    setup();
    uint8_t pkt[ADSP_HEADER_SIZE + ADSP_OPEN_PARAMS_SIZE] = {0};
    pkt[0] = 0x12;
    pkt[1] = 0x34; // source ConnID
    pkt[11] = 0x40; // PktRecvWdw = 64
    pkt[12] = ADSP_DESC_CONTROL | ADSP_CTL_OPEN_REQ;
    pkt[13] = 0x02;
    pkt[14] = 0x00; // version $0200 — not the one this chapter documents
    atalk_socket_addr_t from = {.net = 0, .node = NODE_A, .socket = SOCK_A};
    adsp_input(g_b, &from, SOCK_B, pkt, sizeof(pkt));

    ASSERT_EQ_INT(conn_count(g_b), 0);
    ASSERT_EQ_INT(wire_pending(), 1);
    const wire_pkt_t *w = &g_wire[g_wire_head];
    ASSERT_EQ_INT(w->buf[12], ADSP_DESC_CONTROL | ADSP_CTL_OPEN_DENY); // $84
    // A denial carries source ConnID 0 and the requester's ConnID (12-27).
    ASSERT_EQ_INT((int)((w->buf[0] << 8) | w->buf[1]), 0);
    ASSERT_EQ_INT((int)((w->buf[15] << 8) | w->buf[16]), 0x1234);
    pump();
}

// Reserved control codes $9..$F are rejected (12-14).
TEST(test_reserved_control_code_rejected) {
    setup();
    open_a_to_b();
    adsp_conn_t *cb = first_conn(g_b);
    uint32_t recv_before = adsp_conn_recv_seq(cb);

    uint8_t pkt[ADSP_HEADER_SIZE] = {0};
    pkt[0] = (uint8_t)(adsp_conn_remote_cid(cb) >> 8);
    pkt[1] = (uint8_t)adsp_conn_remote_cid(cb);
    pkt[12] = ADSP_DESC_CONTROL | 0x0F;
    atalk_socket_addr_t from = {.net = 0, .node = NODE_A, .socket = SOCK_A};
    adsp_input(g_b, &from, SOCK_B, pkt, sizeof(pkt));

    ASSERT_EQ_INT((int)adsp_conn_recv_seq(cb), (int)recv_before);
    ASSERT_EQ_INT(conn_count(g_b), 1); // rejected, not fatal
    pump();
}

// A message travels intact and the end-of-message marker consumes exactly one
// sequence number beyond the last byte (12-9).
TEST(test_data_with_eom) {
    setup();
    adsp_conn_t *ca = open_a_to_b();
    const char *msg = "hello";
    ASSERT_EQ_INT(adsp_write(g_a, ca, (const uint8_t *)msg, 5, true), 5);
    pump();

    adsp_conn_t *cb = first_conn(g_b);
    ASSERT_EQ_INT(g_rec_b.stream_len, 5);
    ASSERT_TRUE(memcmp(g_rec_b.stream, msg, 5) == 0);
    ASSERT_EQ_INT(g_rec_b.eom_events, 1);
    ASSERT_EQ_INT((int)adsp_conn_recv_seq(cb), 6); // 5 bytes + the EOM marker
    ASSERT_EQ_INT((int)adsp_conn_send_seq(ca), 6);
    ASSERT_EQ_INT(adsp_conn_unacked(ca), 0); // the acknowledgment came back
    ASSERT_EQ_INT((int)adsp_conn_bytes_out(ca), 5);
    ASSERT_EQ_INT((int)adsp_conn_bytes_in(cb), 5);
}

// An EOM packet may carry no data at all (12-9).
TEST(test_bare_eom_packet) {
    setup();
    adsp_conn_t *ca = open_a_to_b();
    ASSERT_EQ_INT(adsp_write(g_a, ca, (const uint8_t *)"ab", 2, false), 2);
    pump();
    ASSERT_EQ_INT(adsp_write(g_a, ca, NULL, 0, true), 0);
    pump();

    ASSERT_EQ_INT(g_rec_b.stream_len, 2);
    ASSERT_EQ_INT(g_rec_b.eom_events, 1);
    ASSERT_EQ_INT((int)adsp_conn_recv_seq(first_conn(g_b)), 3);
}

// A payload larger than one packet is split at ADSP_MAX_DATA and reassembled
// in order (12-12).
TEST(test_multi_packet_stream) {
    setup();
    adsp_conn_t *ca = open_a_to_b();
    uint8_t payload[1500];
    for (int i = 0; i < (int)sizeof(payload); i++)
        payload[i] = (uint8_t)(i & 0xFF);
    ASSERT_EQ_INT(adsp_write(g_a, ca, payload, (int)sizeof(payload), true), (int)sizeof(payload));
    pump();

    ASSERT_EQ_INT(g_rec_b.stream_len, (int)sizeof(payload));
    ASSERT_TRUE(memcmp(g_rec_b.stream, payload, sizeof(payload)) == 0);
    ASSERT_TRUE(g_rec_b.data_events >= 3); // 1500 bytes needs three packets
    ASSERT_EQ_INT(g_rec_b.eom_events, 1);
    ASSERT_EQ_INT((int)adsp_conn_recv_seq(first_conn(g_b)), (int)sizeof(payload) + 1);
}

// Figure 12-4: the first data packet is lost, the retransmit timer fires, and
// the whole unacknowledged run is resent.
TEST(test_retransmit_after_loss) {
    setup();
    adsp_conn_t *ca = open_a_to_b();
    g_drop_data = 1;
    ASSERT_EQ_INT(adsp_write(g_a, ca, (const uint8_t *)"payload", 7, true), 7);
    pump();
    ASSERT_EQ_INT(g_dropped, 1);
    ASSERT_EQ_INT(g_rec_b.stream_len, 0); // nothing arrived
    ASSERT_EQ_INT(adsp_conn_unacked(ca), 8);

    advance(ADSP_RETRANSMIT_NS + 1);

    ASSERT_EQ_INT(g_rec_b.stream_len, 7);
    ASSERT_TRUE(memcmp(g_rec_b.stream, "payload", 7) == 0);
    ASSERT_EQ_INT(g_rec_b.eom_events, 1);
    ASSERT_EQ_INT(adsp_conn_unacked(ca), 0);
    ASSERT_TRUE(adsp_conn_retransmits(ca) >= 1);
    ASSERT_TRUE(adsp_get_stats(g_a)->retransmits >= 1);
}

// A gap in the stream is refused as out of sequence: in-order acceptance only
// (12-7), and repeated gaps draw a Retransmit Advice (12-14).
TEST(test_out_of_sequence_data_rejected) {
    setup();
    open_a_to_b();
    adsp_conn_t *cb = first_conn(g_b);
    uint16_t cid = adsp_conn_remote_cid(cb);
    atalk_socket_addr_t from = {.net = 0, .node = NODE_A, .socket = SOCK_A};

    for (int i = 0; i < ADSP_OOS_ADVICE_THRESHOLD; i++) {
        uint8_t pkt[ADSP_HEADER_SIZE + 4] = {0};
        pkt[0] = (uint8_t)(cid >> 8);
        pkt[1] = (uint8_t)cid;
        pkt[5] = 40; // PktFirstByteSeq = 40, but RecvSeq is 0
        pkt[11] = 0x40;
        pkt[12] = 0; // plain data, no ack request
        memcpy(&pkt[ADSP_HEADER_SIZE], "junk", 4);
        adsp_input(g_b, &from, SOCK_B, pkt, sizeof(pkt));
    }

    ASSERT_EQ_INT(g_rec_b.stream_len, 0);
    ASSERT_EQ_INT((int)adsp_conn_recv_seq(cb), 0);
    ASSERT_TRUE(adsp_get_stats(g_b)->out_of_sequence >= (uint64_t)ADSP_OOS_ADVICE_THRESHOLD);
    // The last of the run should have produced a Retransmit Advice.
    bool advice = false;
    for (int i = g_wire_head; i < g_wire_tail; i++) {
        if (g_wire[i].buf[12] == (ADSP_DESC_CONTROL | ADSP_CTL_RETRANSMIT))
            advice = true;
    }
    ASSERT_TRUE(advice);
    pump();
}

// Attention messages ride their own sequence space and are acknowledged
// separately (12-19 … 12-21).
TEST(test_attention_message) {
    setup();
    adsp_conn_t *ca = open_a_to_b();
    ASSERT_EQ_INT(adsp_send_attention(g_a, ca, 0x1234, (const uint8_t *)"ping", 4), 0);
    pump();

    ASSERT_EQ_INT(g_rec_b.attn_events, 1);
    ASSERT_EQ_INT((int)g_rec_b.attn_code, 0x1234);
    ASSERT_TRUE(strcmp(g_rec_b.attn_payload, "ping") == 0);
    ASSERT_EQ_INT((int)adsp_get_stats(g_a)->attentions_out, 1);
    ASSERT_EQ_INT((int)adsp_get_stats(g_b)->attentions_in, 1);
    // The acknowledgment cleared the outstanding message, so another may go.
    ASSERT_EQ_INT(adsp_send_attention(g_a, ca, 0x0001, NULL, 0), 0);
    pump();
    ASSERT_EQ_INT(g_rec_b.attn_events, 2);
}

// Codes $F000..$FFFF are reserved by ADSP (12-19), and only one attention
// message may be outstanding at a time (12-21).
TEST(test_attention_limits) {
    setup();
    adsp_conn_t *ca = open_a_to_b();
    ASSERT_EQ_INT(adsp_send_attention(g_a, ca, 0xF001, NULL, 0), -1);
    g_partition = true; // the acknowledgment never comes back
    ASSERT_EQ_INT(adsp_send_attention(g_a, ca, 0x0002, NULL, 0), 0);
    pump();
    ASSERT_EQ_INT(adsp_send_attention(g_a, ca, 0x0003, NULL, 0), -1);
}

// Closing sends the advisory Close Connection packet and frees both ends
// (12-38).
TEST(test_close_advice) {
    setup();
    adsp_conn_t *ca = open_a_to_b();
    adsp_close(g_a, ca, "done");
    ASSERT_EQ_INT(conn_count(g_a), 0);
    ASSERT_EQ_INT(g_rec_a.closes, 1);
    pump();

    ASSERT_EQ_INT(conn_count(g_b), 0);
    ASSERT_EQ_INT(g_rec_b.closes, 1);
    ASSERT_TRUE(strstr(g_rec_b.last_close, "remote end") != NULL);
}

// Figure 12-6: nothing comes back, so the end probes and, on the fourth
// expiry of the connection timer, tears itself down (12-5).
TEST(test_connection_timer_teardown) {
    setup();
    open_a_to_b();
    g_partition = true;

    for (int i = 0; i < ADSP_PROBE_LIMIT - 1; i++) {
        advance(ADSP_PROBE_INTERVAL_NS + 1);
        ASSERT_EQ_INT(conn_count(g_a), 1); // still probing
    }
    advance(ADSP_PROBE_INTERVAL_NS + 1);

    ASSERT_EQ_INT(conn_count(g_a), 0);
    ASSERT_EQ_INT(g_rec_a.closes, 1);
    ASSERT_TRUE(strstr(g_rec_a.last_close, "connection timer") != NULL);
    ASSERT_EQ_INT((int)adsp_get_stats(g_a)->timeouts, 1);
}

// An open request that is never answered is retried a bounded number of times
// and then reported (12-30).
TEST(test_open_request_retries_then_fails) {
    setup();
    g_partition = true;
    atalk_socket_addr_t dest = {.net = 0, .node = NODE_B, .socket = SOCK_B};
    adsp_conn_t *c = adsp_open(g_a, &dest, SOCK_A, &g_client, &g_rec_a);
    ASSERT_TRUE(c != NULL);
    pump();

    for (int i = 1; i < ADSP_OPEN_RETRY_LIMIT; i++) {
        advance(ADSP_OPEN_RETRY_NS + 1);
        ASSERT_EQ_INT(conn_count(g_a), 1);
    }
    advance(ADSP_OPEN_RETRY_NS + 1);

    ASSERT_EQ_INT(conn_count(g_a), 0);
    ASSERT_EQ_INT(g_rec_a.closes, 1);
    ASSERT_TRUE(strstr(g_rec_a.last_close, "never answered") != NULL);
}

// A retransmitted open request must draw the acknowledgment again rather than
// a second connection (12-30).
TEST(test_duplicate_open_request) {
    setup();
    open_a_to_b();
    adsp_conn_t *cb = first_conn(g_b);
    uint16_t peer_cid = adsp_conn_remote_cid(cb);

    uint8_t pkt[ADSP_HEADER_SIZE + ADSP_OPEN_PARAMS_SIZE] = {0};
    pkt[0] = (uint8_t)(peer_cid >> 8);
    pkt[1] = (uint8_t)peer_cid;
    pkt[11] = 0x40;
    pkt[12] = ADSP_DESC_CONTROL | ADSP_CTL_OPEN_REQ;
    pkt[13] = (uint8_t)(ADSP_VERSION >> 8);
    pkt[14] = (uint8_t)ADSP_VERSION;
    atalk_socket_addr_t from = {.net = 0, .node = NODE_A, .socket = SOCK_A};
    adsp_input(g_b, &from, SOCK_B, pkt, sizeof(pkt));

    ASSERT_EQ_INT(conn_count(g_b), 1); // no duplicate end
    ASSERT_EQ_INT(g_rec_b.opens, 1);
    ASSERT_EQ_INT(wire_pending(), 1);
    ASSERT_EQ_INT(g_wire[g_wire_head].buf[12], ADSP_DESC_CONTROL | ADSP_CTL_OPEN_ACK); // $82
    pump();
}

// A forward reset flushes what is still queued and resynchronises both ends
// (12-9, 12-10).
TEST(test_forward_reset) {
    setup();
    adsp_conn_t *ca = open_a_to_b();
    g_drop_data = 1;
    ASSERT_EQ_INT(adsp_write(g_a, ca, (const uint8_t *)"lost bytes", 10, false), 10);
    pump();
    ASSERT_EQ_INT(g_rec_b.stream_len, 0);

    ASSERT_EQ_INT(adsp_forward_reset(g_a, ca), 0);
    pump();

    adsp_conn_t *cb = first_conn(g_b);
    // Both ends now agree on the resynchronised sequence number.
    ASSERT_EQ_INT((int)adsp_conn_recv_seq(cb), (int)adsp_conn_send_seq(ca));
    ASSERT_EQ_INT(adsp_conn_unacked(ca), 0);
    ASSERT_TRUE(adsp_get_stats(g_a)->forward_resets >= 1);

    // The stream still works afterwards.
    ASSERT_EQ_INT(adsp_write(g_a, ca, (const uint8_t *)"ok", 2, true), 2);
    pump();
    ASSERT_EQ_INT(g_rec_b.stream_len, 2);
    ASSERT_TRUE(memcmp(g_rec_b.stream, "ok", 2) == 0);
}

// Figure 12-5: an idle connection probes, and the peer's answer keeps both
// ends alive indefinitely.
TEST(test_idle_probe_keeps_connection) {
    setup();
    adsp_conn_t *ca = open_a_to_b();
    for (int i = 0; i < 10; i++)
        advance(ADSP_PROBE_INTERVAL_NS + 1);

    ASSERT_EQ_INT(conn_count(g_a), 1);
    ASSERT_EQ_INT(conn_count(g_b), 1);
    ASSERT_EQ_INT(g_rec_a.closes, 0);
    ASSERT_EQ_INT((int)adsp_conn_state(ca), (int)ADSP_STATE_OPEN);
}

// The engine refuses to queue more than it can hold rather than dropping
// bytes silently — the flow-control contract of 12-8 seen from the client.
TEST(test_send_queue_backpressure) {
    setup();
    adsp_conn_t *ca = open_a_to_b();
    g_partition = true; // no acknowledgments, so nothing ever drains
    static uint8_t chunk[1024];
    int accepted = 0;
    for (int i = 0; i < (ADSP_SEND_QUEUE / (int)sizeof(chunk)) + 2; i++) {
        if (adsp_write(g_a, ca, chunk, (int)sizeof(chunk), false) < 0)
            break;
        accepted += (int)sizeof(chunk);
    }
    ASSERT_EQ_INT(accepted, ADSP_SEND_QUEUE);
    ASSERT_EQ_INT(adsp_write(g_a, ca, chunk, 1, false), -1);
    // The peer's advertised window bounds what actually went out (12-8).
    ASSERT_TRUE(adsp_conn_unacked(ca) <= ADSP_RECV_WINDOW);
}

// Two connections from the same socket to different peers coexist (12-5).
TEST(test_two_connections_on_one_socket) {
    setup();
    ASSERT_EQ_INT(adsp_listen(g_b, 11, &g_client, &g_rec_b), 0);
    atalk_socket_addr_t d1 = {.net = 0, .node = NODE_B, .socket = SOCK_B};
    atalk_socket_addr_t d2 = {.net = 0, .node = NODE_B, .socket = 11};
    adsp_conn_t *c1 = adsp_open(g_a, &d1, SOCK_A, &g_client, &g_rec_a);
    adsp_conn_t *c2 = adsp_open(g_a, &d2, SOCK_A, &g_client, &g_rec_a);
    pump();

    ASSERT_EQ_INT(conn_count(g_a), 2);
    ASSERT_EQ_INT(conn_count(g_b), 2);
    ASSERT_TRUE(adsp_conn_local_cid(c1) != adsp_conn_local_cid(c2));

    ASSERT_EQ_INT(adsp_write(g_a, c1, (const uint8_t *)"one", 3, true), 3);
    ASSERT_EQ_INT(adsp_write(g_a, c2, (const uint8_t *)"two", 3, true), 3);
    pump();
    ASSERT_EQ_INT(g_rec_b.stream_len, 6);
}

int main(void) {
    RUN(test_open_request_wire_format);
    RUN(test_open_dialog);
    RUN(test_simultaneous_open);
    RUN(test_open_denied_without_listener);
    RUN(test_open_refused_by_client);
    RUN(test_open_version_mismatch_denied);
    RUN(test_reserved_control_code_rejected);
    RUN(test_data_with_eom);
    RUN(test_bare_eom_packet);
    RUN(test_multi_packet_stream);
    RUN(test_retransmit_after_loss);
    RUN(test_out_of_sequence_data_rejected);
    RUN(test_attention_message);
    RUN(test_attention_limits);
    RUN(test_close_advice);
    RUN(test_connection_timer_teardown);
    RUN(test_open_request_retries_then_fails);
    RUN(test_duplicate_open_request);
    RUN(test_forward_reset);
    RUN(test_idle_probe_keeps_connection);
    RUN(test_send_queue_backpressure);
    RUN(test_two_connections_on_one_socket);
    printf("adsp: all tests passed\n");
    return 0;
}
