// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the mailbox (mailbox.c): a client writes REQ_EVAL records
// into the request ring and reads EVT_RESULT records from the event ring,
// in the same process, while gs_mailbox_drain serves them through a stub
// evaluator that echoes what it was asked.  The rings are 4 KB (the suite
// Makefile overrides the sizes) so wraps and a held-back result are cheap
// to force.

#include "common.h"
#include "test_assert.h"
#include "mailbox/mailbox.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *g_region;
static gs_mailbox_t g_m;
static volatile uint32_t *g_ctrl;
static mbx_ring_t g_req, g_evt; // the client's views
static int g_notified;
static int g_evals;
static char g_last_path[256];
static char g_last_args[256];

void gs_mailbox_notify(volatile uint32_t *word) {
    (void)word;
    g_notified++;
}

// The stub leaf: answers {"path": <path>, "args": <args or null>}; a path
// starting with "fail" returns -1 with an error document; a path starting
// with "big:N" answers N bytes of 'x'.
static int stub_eval(const char *path, const char *args, char *out, size_t out_size) {
    g_evals++;
    snprintf(g_last_path, sizeof g_last_path, "%s", path);
    snprintf(g_last_args, sizeof g_last_args, "%s", args ? args : "(none)");
    if (strncmp(path, "fail", 4) == 0) {
        snprintf(out, out_size, "{\"error\":\"%s\"}", path);
        return -1;
    }
    if (strncmp(path, "big:", 4) == 0) {
        size_t n = (size_t)atoi(path + 4);
        if (n >= out_size)
            n = out_size - 1;
        memset(out, 'x', n);
        out[n] = '\0';
        return 0;
    }
    snprintf(out, out_size, "{\"path\":\"%s\",\"args\":%s}", path, args ? args : "null");
    return 0;
}

static void fresh(void) {
    free(g_region);
    size_t bytes = gs_mailbox_region_bytes(GS_MBX_REQ_BYTES, GS_MBX_EVT_BYTES);
    g_region = (uint8_t *)malloc(bytes);
    gs_mailbox_free(&g_m);
    g_ctrl = gs_mailbox_init(&g_m, g_region, GS_MBX_REQ_BYTES, GS_MBX_EVT_BYTES, stub_eval);
    ASSERT_TRUE(g_ctrl != NULL);
    uint8_t *base = (uint8_t *)g_ctrl;
    mbx_ring_init(&g_req, g_ctrl, GS_MBX_C_REQ_HEAD, GS_MBX_C_REQ_TAIL, base + g_ctrl[GS_MBX_C_REQ_OFF],
                  g_ctrl[GS_MBX_C_REQ_SIZE]);
    mbx_ring_init(&g_evt, g_ctrl, GS_MBX_C_EVT_HEAD, GS_MBX_C_EVT_TAIL, base + g_ctrl[GS_MBX_C_EVT_OFF],
                  g_ctrl[GS_MBX_C_EVT_SIZE]);
    g_notified = 0;
    g_evals = 0;
}

// The client writes one REQ_EVAL and publishes it.  False when no room.
static bool post(uint32_t id, const char *path, const char *args) {
    uint32_t plen = (uint32_t)strlen(path), alen = args ? (uint32_t)strlen(args) : 0;
    uint32_t len = MBX_HDR_BYTES + 4u * GS_MBX_EVAL_WORDS + ((plen + 3u) & ~3u) + ((alen + 3u) & ~3u);
    uint32_t at = mbx_reserve(&g_req, GS_MBX_REQ_EVAL, len);
    if (at == UINT32_MAX)
        return false;
    uint8_t *p = mbx_payload(&g_req, at);
    WR_LE32(p + 4 * GS_MBX_EVAL_ID, id);
    WR_LE32(p + 4 * GS_MBX_EVAL_CLIENT, 1);
    WR_LE32(p + 4 * GS_MBX_EVAL_DEADLINE, 30000);
    WR_LE32(p + 4 * GS_MBX_EVAL_PATH_LEN, plen);
    WR_LE32(p + 4 * GS_MBX_EVAL_ARGS_LEN, alen);
    memcpy(p + 4 * GS_MBX_EVAL_WORDS, path, plen);
    if (alen)
        memcpy(p + 4 * GS_MBX_EVAL_WORDS + ((plen + 3u) & ~3u), args, alen);
    mbx_publish(&g_req);
    return true;
}

// The client reads the next EVT_RESULT (skipping PADs) into id/ok/json.
// Returns 1, 0 (none) or -1 (corrupt).
static int take(uint32_t *id, uint32_t *ok, char *json, size_t cap) {
    for (;;) {
        uint32_t head = mbx_load(g_ctrl, GS_MBX_C_EVT_HEAD);
        mbx_rec_t rec;
        int got = mbx_next(&g_evt, head, &rec);
        if (got <= 0)
            return got;
        if (rec.kind == MBX_R_PAD) {
            mbx_consume(&g_evt, &rec);
            continue;
        }
        ASSERT_EQ_INT(rec.kind, GS_MBX_EVT_RESULT);
        const uint8_t *p = mbx_rec_payload(&g_evt, &rec);
        *id = RD_LE32(p + 4 * GS_MBX_RESULT_ID);
        *ok = RD_LE32(p + 4 * GS_MBX_RESULT_OK);
        uint32_t n = RD_LE32(p + 4 * GS_MBX_RESULT_JSON_LEN);
        if (n >= cap)
            n = (uint32_t)cap - 1;
        memcpy(json, p + 4 * GS_MBX_RESULT_WORDS, n);
        json[n] = '\0';
        mbx_consume(&g_evt, &rec);
        return 1;
    }
}

// The client reads the next record of any kind (skipping PADs): returns its
// kind (0: none), the payload copied into buf.
static uint32_t take_any(uint8_t *buf, size_t cap, uint32_t *len) {
    for (;;) {
        uint32_t head = mbx_load(g_ctrl, GS_MBX_C_EVT_HEAD);
        mbx_rec_t rec;
        int got = mbx_next(&g_evt, head, &rec);
        if (got <= 0)
            return 0;
        if (rec.kind == MBX_R_PAD) {
            mbx_consume(&g_evt, &rec);
            continue;
        }
        *len = rec.len - MBX_HDR_BYTES;
        memcpy(buf, mbx_rec_payload(&g_evt, &rec), *len < cap ? *len : cap);
        mbx_consume(&g_evt, &rec);
        return rec.kind;
    }
}

// A leaf that emits an event while it runs, the way debug.step emits
// mode_ended before its own result is written.
static uint32_t g_client_seen;
static int emitting_eval(const char *path, const char *args, char *out, size_t out_size) {
    g_client_seen = gs_mailbox_current_client(&g_m);
    gs_mailbox_emit(&g_m, GS_MBX_EVT_STATE, "{\"event\":\"mode_ended\"}");
    return stub_eval(path, args, out, out_size);
}

static double fake_now_us(void) {
    static double t = 0;
    t += 100.0; // every call is 100 us later
    return t;
}

TEST(the_control_block_is_laid_out_and_versioned) {
    fresh();
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_MAGIC], GS_MAILBOX_MAGIC);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_VERSION], GS_MAILBOX_VERSION);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_REQ_SIZE], 4096);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_EVT_SIZE], 4096);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_EVT_OFF], GS_MBX_CTRL_WORDS * 4 + 4096);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_STATUS], GS_MBX_STATUS_ATTACHED);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_READY], 0);
    ASSERT_TRUE(((uintptr_t)g_ctrl & 63u) == 0);
    gs_mailbox_set_ready(&g_m);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_READY], 1);
    ASSERT_EQ_INT(g_notified, 1);
    gs_mailbox_heartbeat(&g_m);
    gs_mailbox_heartbeat(&g_m);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_HEARTBEAT], 2);
}

TEST(a_request_is_served_and_its_id_comes_back) {
    fresh();
    ASSERT_TRUE(!gs_mailbox_has_requests(&g_m));
    ASSERT_TRUE(post(17, "machine.cpu.pc", NULL));
    ASSERT_TRUE(gs_mailbox_has_requests(&g_m));
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 1);
    ASSERT_EQ_INT(g_evals, 1);
    ASSERT_TRUE(strcmp(g_last_path, "machine.cpu.pc") == 0);
    ASSERT_TRUE(strcmp(g_last_args, "(none)") == 0);
    uint32_t id, ok;
    char json[512];
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 17);
    ASSERT_EQ_INT(ok, 1);
    ASSERT_TRUE(strcmp(json, "{\"path\":\"machine.cpu.pc\",\"args\":null}") == 0);
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 0);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_STAT_REQUESTS], 1);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_STAT_EVENTS], 1);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_REQ_TAIL], g_ctrl[GS_MBX_C_REQ_HEAD]);
}

TEST(arguments_travel_and_failure_is_ok_zero) {
    fresh();
    ASSERT_TRUE(post(1, "storage.list_dir", "[\"/opfs\"]"));
    ASSERT_TRUE(post(2, "fail.this", "{\"k\":1}"));
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 2);
    uint32_t id, ok;
    char json[512];
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 1);
    ASSERT_EQ_INT(ok, 1);
    ASSERT_TRUE(strcmp(json, "{\"path\":\"storage.list_dir\",\"args\":[\"/opfs\"]}") == 0);
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 2);
    ASSERT_EQ_INT(ok, 0);
    ASSERT_TRUE(strcmp(json, "{\"error\":\"fail.this\"}") == 0);
}

TEST(many_requests_are_served_in_one_drain_in_order) {
    fresh();
    int posted = 0;
    for (uint32_t i = 0; i < 200; i++) {
        char path[32];
        snprintf(path, sizeof path, "p%u", i);
        if (!post(100 + i, path, NULL))
            break;
        posted++;
    }
    ASSERT_TRUE(posted > 40); // 4 KB of 32-byte records: the ring is full
    // The answers (~60 bytes each) do not all fit the 4 KB event ring at
    // once, so the drain serves as many as fit, holds the next, and the
    // client's reads let the rest through -- in order throughout.
    uint32_t id, ok;
    char json[512];
    int served = 0;
    int drains = 0;
    while (served < posted) {
        int n = gs_mailbox_drain(&g_m, 0, NULL);
        ASSERT_TRUE(n > 0);
        drains++;
        for (int k = 0; k < n; k++) {
            int got = take(&id, &ok, json, sizeof json);
            ASSERT_EQ_INT(got, 1);
            ASSERT_EQ_INT(id, 100 + (uint32_t)served);
            served++;
        }
    }
    ASSERT_TRUE(drains > 1);
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 0);
    ASSERT_EQ_INT(g_evals, posted);
    // The request ring is empty again and the client can post a full round more.
    for (uint32_t i = 0; i < (uint32_t)posted; i++)
        ASSERT_TRUE(post(500 + i, "q", NULL));
}

TEST(the_budget_ends_a_drain_between_leaves_not_inside_one) {
    fresh();
    for (uint32_t i = 0; i < 10; i++)
        ASSERT_TRUE(post(i, "p", NULL));
    // fake_now_us advances 100 us per call; the drain calls it once at the
    // start and once after each leaf: a 250 us budget serves three.
    int n = gs_mailbox_drain(&g_m, 250.0, fake_now_us);
    ASSERT_EQ_INT(n, 3);
    ASSERT_TRUE(gs_mailbox_has_requests(&g_m));
    n = gs_mailbox_drain(&g_m, 0, NULL); // no budget: the rest
    ASSERT_EQ_INT(n, 7);
    ASSERT_TRUE(g_ctrl[GS_MBX_C_STAT_DRAIN_US] > 0);
}

TEST(a_result_with_no_room_is_held_and_delivered_when_the_client_reads) {
    fresh();
    // Two 1,800-byte answers fill the 4 KB event ring; the third is held.
    ASSERT_TRUE(post(1, "big:1800", NULL));
    ASSERT_TRUE(post(2, "big:1800", NULL));
    ASSERT_TRUE(post(3, "big:1800", NULL));
    ASSERT_TRUE(post(4, "small", NULL));
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 2);
    ASSERT_TRUE(g_m.held);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_STAT_STALLS], 1);
    ASSERT_EQ_INT(g_evals, 3); // the third leaf ran; only its delivery waits
    // Nothing more is served until the held answer goes out.
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 0);
    ASSERT_EQ_INT(g_evals, 3);
    // The client reads one answer (1,824 bytes freed).  The held answer
    // needs a 448-byte PAD to the ring's end plus 1,824 at the start: it
    // fits exactly and fills the ring, so the fourth is served but held.
    uint32_t id, ok;
    char json[2048];
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 1);
    ASSERT_EQ_INT((int)strlen(json), 1800);
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 1);
    ASSERT_TRUE(g_m.held);
    ASSERT_EQ_INT(g_evals, 4);
    // Reading the second frees room for the fourth.
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 2);
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 1);
    ASSERT_TRUE(!g_m.held);
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 3);
    ASSERT_EQ_INT((int)strlen(json), 1800);
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 4);
    ASSERT_EQ_INT(ok, 1);
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 0);
}

TEST(a_malformed_request_is_answered_not_dropped) {
    fresh();
    // A REQ_EVAL whose path_len exceeds the record.
    uint32_t at = mbx_reserve(&g_req, GS_MBX_REQ_EVAL, MBX_HDR_BYTES + 4u * GS_MBX_EVAL_WORDS + 8);
    uint8_t *p = mbx_payload(&g_req, at);
    WR_LE32(p + 4 * GS_MBX_EVAL_ID, 9);
    WR_LE32(p + 4 * GS_MBX_EVAL_CLIENT, 1);
    WR_LE32(p + 4 * GS_MBX_EVAL_DEADLINE, 0);
    WR_LE32(p + 4 * GS_MBX_EVAL_PATH_LEN, 500);
    WR_LE32(p + 4 * GS_MBX_EVAL_ARGS_LEN, 0);
    mbx_publish(&g_req);
    // And a kind this build does not serve, with an id.
    at = mbx_reserve(&g_req, GS_MBX_REQ_SCRIPT, MBX_HDR_BYTES + 4);
    WR_LE32(mbx_payload(&g_req, at), 10);
    mbx_publish(&g_req);
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 2);
    ASSERT_EQ_INT(g_evals, 0);
    uint32_t id, ok;
    char json[512];
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 9);
    ASSERT_EQ_INT(ok, 0);
    ASSERT_TRUE(strstr(json, "lengths exceed") != NULL);
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 10);
    ASSERT_TRUE(strstr(json, "unsupported request kind 2") != NULL);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_STAT_BAD], 2);
}

TEST(corrupt_framing_marks_the_mailbox_lost) {
    fresh();
    ASSERT_TRUE(post(1, "p", NULL));
    // Break the record's len.
    WR_LE32(g_req.buf + 4, 12);
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 0);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_STATUS], GS_MBX_STATUS_LOST);
    ASSERT_TRUE(g_notified >= 1);
    // Lost is for good: a later good request is not served.
    ASSERT_TRUE(post(2, "p", NULL));
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 0);
    ASSERT_EQ_INT(g_evals, 0);
}

TEST(both_rings_wrap_across_thousands_of_round_trips) {
    fresh();
    uint32_t next = 1;
    uint32_t id, ok;
    char json[512];
    for (int round = 0; round < 3000; round++) {
        char path[48];
        snprintf(path, sizeof path, "path.number.%u", next);
        char args[48];
        snprintf(args, sizeof args, "[%u,\"%s\"]", next, (next & 1) ? "odd" : "even");
        ASSERT_TRUE(post(next, path, args));
        ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 1);
        ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
        ASSERT_EQ_INT(id, next);
        char want[160];
        snprintf(want, sizeof want, "{\"path\":\"%s\",\"args\":%s}", path, args);
        ASSERT_TRUE(strcmp(json, want) == 0);
        next++;
    }
    ASSERT_TRUE(g_req.wr > 4096u * 8u); // both rings wrapped many times
    ASSERT_TRUE(g_evt.rd > 4096u * 8u);
}

TEST(an_event_is_published_at_once_and_ordered_before_the_result) {
    fresh();
    ASSERT_EQ_INT(gs_mailbox_current_client(&g_m), 0);
    // Emitted from the tick (no request in flight): visible without a drain.
    ASSERT_TRUE(gs_mailbox_emit(&g_m, GS_MBX_EVT_NOTIFY, "{\"event\":\"floppy\"}"));
    ASSERT_EQ_INT(g_notified, 1);
    uint8_t buf[256];
    uint32_t len;
    ASSERT_EQ_INT(take_any(buf, sizeof buf, &len), GS_MBX_EVT_NOTIFY);
    ASSERT_EQ_INT(RD_LE32(buf + 4 * GS_MBX_EVENT_JSON_LEN), 18);
    ASSERT_TRUE(memcmp(buf + 4 * GS_MBX_EVENT_WORDS, "{\"event\":\"floppy\"}", 18) == 0);
    // Emitted inside a leaf: the event precedes that leaf's result, and the
    // leaf saw the requesting client.
    g_m.eval = emitting_eval;
    ASSERT_TRUE(post(5, "debug.step", NULL));
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 1);
    ASSERT_EQ_INT(g_client_seen, 1);
    ASSERT_EQ_INT(gs_mailbox_current_client(&g_m), 0);
    ASSERT_EQ_INT(take_any(buf, sizeof buf, &len), GS_MBX_EVT_STATE);
    ASSERT_EQ_INT(take_any(buf, sizeof buf, &len), GS_MBX_EVT_RESULT);
    ASSERT_EQ_INT(RD_LE32(buf + 4 * GS_MBX_RESULT_ID), 5);
    ASSERT_EQ_INT(take_any(buf, sizeof buf, &len), 0);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_STAT_EVENTS], 3);
}

TEST(an_event_with_no_room_is_dropped_and_counted_never_blocking) {
    fresh();
    char big[2000];
    memset(big, 'e', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    int written = 0;
    while (gs_mailbox_emit(&g_m, GS_MBX_EVT_LOG, big))
        written++;
    ASSERT_TRUE(written >= 1);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_STAT_DROPPED], 1);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_STAT_EVENTS], written);
    // The client reads one; the next emit fits again.
    uint8_t buf[8];
    uint32_t len;
    ASSERT_EQ_INT(take_any(buf, sizeof buf, &len), GS_MBX_EVT_LOG);
    ASSERT_TRUE(gs_mailbox_emit(&g_m, GS_MBX_EVT_LOG, big));
}

int main(void) {
    RUN(the_control_block_is_laid_out_and_versioned);
    RUN(a_request_is_served_and_its_id_comes_back);
    RUN(arguments_travel_and_failure_is_ok_zero);
    RUN(many_requests_are_served_in_one_drain_in_order);
    RUN(the_budget_ends_a_drain_between_leaves_not_inside_one);
    RUN(a_result_with_no_room_is_held_and_delivered_when_the_client_reads);
    RUN(a_malformed_request_is_answered_not_dropped);
    RUN(corrupt_framing_marks_the_mailbox_lost);
    RUN(both_rings_wrap_across_thousands_of_round_trips);
    RUN(an_event_is_published_at_once_and_ordered_before_the_result);
    RUN(an_event_with_no_room_is_dropped_and_counted_never_blocking);
    return 0;
}
