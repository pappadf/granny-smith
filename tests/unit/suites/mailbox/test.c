// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the mailbox (mailbox.c): a client writes REQ_EVAL records
// into the request ring and reads EVT_RESULT records from the event ring,
// in the same process, while gs_mailbox_drain serves them through a stub
// evaluator that echoes what it was asked.  The rings are 4 KB (the suite
// Makefile overrides the sizes) so wraps and a held-back result are cheap
// to force.

#include "common.h"
#include "gs_out.h"
#include "test_assert.h"
#include "io/io_worker.h"
#include "job/job.h"
#include "mailbox/mailbox.h"
#include "object/meta.h"
#include "object/object.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

// Annotation bodies are formatted with value_format; the objects it can
// name do not occur here.
const class_desc_t *object_class(const struct object *o) {
    (void)o;
    return NULL;
}
const char *object_name(const struct object *o) {
    (void)o;
    return NULL;
}
void object_compute_path(struct object *o, char *buf, size_t size) {
    (void)o;
    if (size)
        buf[0] = '\0';
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
    job_layer_init(); // this thread plays the emulator thread
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

// The client reads the next EVT_RESULT with its captured output.
static int take_out(uint32_t *id, uint32_t *ok, char *json, size_t cap, char *out, size_t out_cap) {
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
        uint32_t on = RD_LE32(p + 4 * GS_MBX_RESULT_OUT_LEN);
        if (n >= cap)
            n = (uint32_t)cap - 1;
        memcpy(json, p + 4 * GS_MBX_RESULT_WORDS, n);
        json[n] = '\0';
        if (on >= out_cap)
            on = (uint32_t)out_cap - 1;
        memcpy(out, p + 4 * GS_MBX_RESULT_WORDS + ((RD_LE32(p + 4 * GS_MBX_RESULT_JSON_LEN) + 3u) & ~3u), on);
        out[on] = '\0';
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

// The job glue (job.h): scripts "run" by recording their text; a mode is a
// flag per owner the test flips.
static char g_last_script[256];
static int g_scripts;
static uint32_t g_mode_owner; // 0: no mode running
static uint32_t g_mode_id;
static int g_stops;
int job_glue_run_source_threaded(const char *src);
static void print_on_emulator(void *ud) {
    (void)ud;
    gs_outs("two\n"); // the emulator thread, serving the job's call
}
int job_glue_run_source_printing(const char *src) {
    (void)src;
    gs_outf("one %s\n", "\"quoted\""); // the job thread itself
    job_on_emulator(print_on_emulator, NULL);
    return 0;
}
int job_glue_run_source(const char *src, bool interactive) {
    (void)interactive;
    if (strcmp(src, "threaded") == 0)
        return job_glue_run_source_threaded(src);
    if (strcmp(src, "printing") == 0)
        return job_glue_run_source_printing(src);
    g_scripts++;
    snprintf(g_last_script, sizeof g_last_script, "%s", src);
    return strncmp(src, "fail", 4) == 0 ? -1 : 0;
}
bool job_glue_mode_waits(uint32_t client) {
    return g_mode_owner != 0 && g_mode_owner == client;
}
bool job_glue_unbounded_waits(uint32_t client) {
    (void)client;
    return true;
}
uint32_t job_glue_mode_id(void) {
    return g_mode_id;
}
bool job_glue_stop_mode(uint32_t client, uint32_t mode_id) {
    if (g_mode_id != mode_id)
        return false;
    return job_glue_stop_modes(client);
}
bool job_glue_stop_modes(uint32_t client) {
    g_stops++;
    if (g_mode_owner && (client == 0 || g_mode_owner == client)) {
        g_mode_owner = 0;
        return true;
    }
    return false;
}

// The client posts a REQ_SCRIPT / REQ_CANCEL / REQ_MODE_STOP.
static bool post_script(uint32_t id, const char *src) {
    uint32_t n = (uint32_t)strlen(src);
    uint32_t at = mbx_reserve(&g_req, GS_MBX_REQ_SCRIPT, MBX_HDR_BYTES + 4u * GS_MBX_SCRIPT_WORDS + ((n + 3u) & ~3u));
    if (at == UINT32_MAX)
        return false;
    uint8_t *p = mbx_payload(&g_req, at);
    WR_LE32(p + 4 * GS_MBX_SCRIPT_ID, id);
    WR_LE32(p + 4 * GS_MBX_SCRIPT_CLIENT, 2);
    WR_LE32(p + 4 * GS_MBX_SCRIPT_DEADLINE, 0);
    WR_LE32(p + 4 * GS_MBX_SCRIPT_SRC_LEN, n);
    memcpy(p + 4 * GS_MBX_SCRIPT_WORDS, src, n);
    mbx_publish(&g_req);
    return true;
}
static void post_ctl(uint32_t kind, uint32_t id, uint32_t client, uint32_t arg) {
    uint32_t at = mbx_reserve(&g_req, kind, MBX_HDR_BYTES + 4u * GS_MBX_CTL_WORDS);
    ASSERT_TRUE(at != UINT32_MAX);
    uint8_t *p = mbx_payload(&g_req, at);
    WR_LE32(p + 4 * GS_MBX_CTL_ID, id);
    WR_LE32(p + 4 * GS_MBX_CTL_CLIENT, client);
    WR_LE32(p + 4 * GS_MBX_CTL_ARG, arg);
    mbx_publish(&g_req);
}

// A leaf that answers later: it defers, and the test completes it.
static uint32_t g_defer_token;
static int deferring_eval(const char *path, const char *args, char *out, size_t out_size) {
    (void)args;
    if (strncmp(path, "defer", 5) == 0) {
        g_defer_token = gs_result_defer();
        snprintf(out, out_size, "true");
        return 0;
    }
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
    ASSERT_TRUE(post(1, "files.list_dir", "[\"/opfs\"]"));
    ASSERT_TRUE(post(2, "fail.this", "{\"k\":1}"));
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 2);
    uint32_t id, ok;
    char json[512];
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 1);
    ASSERT_EQ_INT(ok, 1);
    ASSERT_TRUE(strcmp(json, "{\"path\":\"files.list_dir\",\"args\":[\"/opfs\"]}") == 0);
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
    at = mbx_reserve(&g_req, 9u, MBX_HDR_BYTES + 4);
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
    ASSERT_TRUE(strstr(json, "unsupported request kind 9") != NULL);
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
    ASSERT_EQ_INT((int)g_ctrl[GS_MBX_C_STAT_EVENTS], written);
    // The client reads one; the next emit fits again.
    uint8_t buf[8];
    uint32_t len;
    ASSERT_EQ_INT(take_any(buf, sizeof buf, &len), GS_MBX_EVT_LOG);
    ASSERT_TRUE(gs_mailbox_emit(&g_m, GS_MBX_EVT_LOG, big));
}

TEST(a_script_without_a_job_thread_runs_inline_and_answers_the_prompt) {
    fresh();
    ASSERT_TRUE(!job_thread_running());
    g_scripts = 0;
    ASSERT_TRUE(post_script(21, "echo hi"));
    ASSERT_TRUE(post_script(22, "fail me"));
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 2);
    ASSERT_EQ_INT(g_scripts, 2);
    ASSERT_TRUE(strcmp(g_last_script, "fail me") == 0);
    uint32_t id, ok;
    char json[512];
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 21);
    ASSERT_EQ_INT(ok, 1);
    // The answer is the prompt: the stub leaf answered "shell.prompt".
    ASSERT_TRUE(strstr(json, "shell.prompt") != NULL);
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 22);
    ASSERT_EQ_INT(ok, 0);
    ASSERT_TRUE(strstr(json, "command failed") != NULL);
}

TEST(cancel_and_mode_stop_are_answered_and_stop_only_the_owner) {
    fresh();
    g_stops = 0;
    g_mode_owner = 2;
    // A stop by another owner leaves the mode alone; the owner's stops it.
    post_ctl(GS_MBX_REQ_MODE_STOP, 31, 1, 1);
    post_ctl(GS_MBX_REQ_MODE_STOP, 32, 1, 2);
    // Cancelling a job that does not exist answers false.
    post_ctl(GS_MBX_REQ_CANCEL, 33, 2, 99);
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 3);
    uint32_t id, ok;
    char json[64];
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 31);
    ASSERT_TRUE(strcmp(json, "false") == 0);
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 32);
    ASSERT_TRUE(strcmp(json, "true") == 0);
    ASSERT_EQ_INT(g_mode_owner, 0);
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 33);
    ASSERT_TRUE(strcmp(json, "false") == 0);
}

// With a real job thread: the script's call reaches the emulator thread
// through the drain, a call that starts a mode holds the job until the
// mode ends, and a cancel unwinds it.
static int g_calls_served;
static void seam_probe(void *ud) {
    g_calls_served++;
    *(uint32_t *)ud = gs_mailbox_current_client(&g_m);
    // "scheduler.run": the leaf opens a mode owned by the caller.
    g_mode_id++;
    g_mode_owner = gs_mailbox_current_client(&g_m);
}
static volatile int g_job_phase;
int job_glue_run_source_threaded(const char *src) {
    (void)src;
    uint32_t client_seen = 0;
    g_job_phase = 1;
    job_on_emulator(seam_probe, &client_seen); // returns once the mode has ended
    g_job_phase = client_seen == 2 ? 2 : -1;
    if (job_current_cancelled())
        return -1;
    return 0;
}

TEST(a_job_thread_calls_the_emulator_through_the_drain_and_waits_for_its_mode) {
    fresh();
    ASSERT_TRUE(job_thread_start(256u << 10));
    g_calls_served = 0;
    g_job_phase = 0;
    g_mode_owner = 0;
    // The glue runs scripts inline in the other tests; here the job thread
    // runs one that posts a call.  Swap the runner by script text.
    ASSERT_TRUE(post_script(41, "threaded"));
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 0); // queued, no answer yet
    // Wait for the job thread to post its call, then serve it.
    for (int i = 0; i < 20000 && !job_layer_has_work(); i++)
        usleep(100);
    ASSERT_TRUE(job_layer_has_work());
    gs_mailbox_drain(&g_m, 0, NULL);
    ASSERT_EQ_INT(g_calls_served, 1);
    ASSERT_EQ_INT(g_mode_owner, 2);
    // The mode runs: the job stays held, no result.
    usleep(2000);
    gs_mailbox_drain(&g_m, 0, NULL);
    ASSERT_EQ_INT(g_job_phase, 1);
    // The mode ends: the next drain releases the job, which finishes.
    g_mode_owner = 0;
    uint32_t id = 0, ok = 0;
    char json[256];
    for (int i = 0; i < 20000 && take(&id, &ok, json, sizeof json) == 0; i++) {
        gs_mailbox_drain(&g_m, 0, NULL);
        usleep(100);
    }
    ASSERT_EQ_INT(id, 41);
    ASSERT_EQ_INT(ok, 1);
    ASSERT_EQ_INT(g_job_phase, 2);
    // Cancel: a queued job finishes at once as cancelled, and stops the
    // client's modes.
    g_stops = 0;
    ASSERT_TRUE(post_script(42, "threaded"));
    post_ctl(GS_MBX_REQ_CANCEL, 43, 2, 42);
    gs_mailbox_drain(&g_m, 0, NULL);
    for (int i = 0; i < 20000 && take(&id, &ok, json, sizeof json) == 0; i++) {
        gs_mailbox_drain(&g_m, 0, NULL);
        usleep(100);
    }
    // The cancel's own answer and the job's come back; order depends on
    // which the drain wrote first.
    uint32_t id2 = 0, ok2 = 0;
    char json2[256];
    for (int i = 0; i < 20000 && take(&id2, &ok2, json2, sizeof json2) == 0; i++) {
        gs_mailbox_drain(&g_m, 0, NULL);
        usleep(100);
    }
    const char *job_json = id == 42 ? json : json2;
    uint32_t job_ok = id == 42 ? ok : ok2;
    ASSERT_TRUE((id == 42 && id2 == 43) || (id == 43 && id2 == 42));
    ASSERT_EQ_INT(job_ok, 0);
    ASSERT_TRUE(strstr(job_json, "cancelled") != NULL);
    // A queued job started no mode, so the cancel stopped none.
    ASSERT_EQ_INT(g_stops, 0);
}

TEST(the_in_process_client_posts_and_reads_like_the_page) {
    fresh();
    gs_mailbox_client_t c;
    gs_mailbox_client_init(&c, &g_m);
    uint32_t id1 = gs_mailbox_client_script(&c, 3, "echo one", 8);
    uint32_t id2 = gs_mailbox_client_mode_stop(&c, 3, 0);
    uint32_t id3 = gs_mailbox_client_cancel(&c, 3, id1);
    ASSERT_TRUE(id1 && id2 && id3 && id1 != id2 && id2 != id3);
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 3); // inline: all answered now
    uint8_t buf[256];
    uint32_t len;
    ASSERT_EQ_INT(gs_mailbox_client_take(&c, buf, sizeof buf, &len), GS_MBX_EVT_RESULT);
    ASSERT_EQ_INT(RD_LE32(buf + 4 * GS_MBX_RESULT_ID), id1);
    ASSERT_EQ_INT(RD_LE32(buf + 4 * GS_MBX_RESULT_OK), 1);
    ASSERT_EQ_INT(gs_mailbox_client_take(&c, buf, sizeof buf, &len), GS_MBX_EVT_RESULT);
    ASSERT_EQ_INT(RD_LE32(buf + 4 * GS_MBX_RESULT_ID), id2);
    ASSERT_EQ_INT(gs_mailbox_client_take(&c, buf, sizeof buf, &len), GS_MBX_EVT_RESULT);
    ASSERT_EQ_INT(RD_LE32(buf + 4 * GS_MBX_RESULT_ID), id3);
    ASSERT_EQ_INT(gs_mailbox_client_take(&c, buf, sizeof buf, &len), 0);
    // Too large a source is refused before the ring.
    ASSERT_EQ_INT(gs_mailbox_client_script(&c, 3, "x", GS_MBX_SCRIPT_MAX + 1), 0);
}

TEST(a_deferred_leaf_answers_when_completed_not_when_served) {
    fresh();
    g_m.eval = deferring_eval;
    g_defer_token = 0;
    ASSERT_TRUE(post(51, "defer", NULL));
    ASSERT_TRUE(post(52, "plain", NULL));
    // The drain served both; only the plain one has an answer now, and the
    // request ring is consumed for both.
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 1);
    ASSERT_TRUE(g_defer_token != 0);
    ASSERT_EQ_INT(g_ctrl[GS_MBX_C_REQ_TAIL], g_ctrl[GS_MBX_C_REQ_HEAD]);
    uint32_t id, ok;
    char json[512];
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 52);
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 0);
    // Completion writes the deferred answer, published at once.
    g_notified = 0;
    gs_result_complete_error(g_defer_token, "disk \"full\"");
    ASSERT_EQ_INT(g_notified, 1);
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 51);
    ASSERT_EQ_INT(ok, 0);
    ASSERT_TRUE(strcmp(json, "{\"error\":\"disk \\\"full\\\"\"}") == 0);
    // Outside any request there is nothing to defer.
    ASSERT_EQ_INT(gs_result_defer(), 0);
    // An unknown token is ignored.
    gs_result_complete_ok(12345);
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 0);
}

// A leaf that prints while it runs (gs_out.h): the text travels with its
// answer when the platform captures output, else to stdout.
static int printing_eval(const char *path, const char *args, char *out, size_t out_size) {
    gs_outf("hello %s\n", path);
    gs_outs("second line\n");
    return stub_eval(path, args, out, out_size);
}

TEST(what_a_leaf_prints_travels_with_its_answer_when_captured) {
    fresh();
    g_m.eval = printing_eval;
    gs_mailbox_set_capture_output(&g_m, true);
    ASSERT_TRUE(post(61, "p", NULL));
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 1);
    uint32_t id, ok;
    char json[128], out[128];
    ASSERT_EQ_INT(take_out(&id, &ok, json, sizeof json, out, sizeof out), 1);
    ASSERT_EQ_INT(id, 61);
    ASSERT_TRUE(strcmp(out, "hello p\nsecond line\n") == 0);
    ASSERT_TRUE(strstr(json, "\"path\":\"p\"") != NULL);
    // Not captured: the answer carries no output (the text went to fd 1).
    gs_mailbox_set_capture_output(&g_m, false);
    ASSERT_TRUE(post(62, "q", NULL));
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 1);
    ASSERT_EQ_INT(take_out(&id, &ok, json, sizeof json, out, sizeof out), 1);
    ASSERT_EQ_INT(id, 62);
    ASSERT_TRUE(out[0] == '\0');
}

TEST(progress_of_a_deferred_leaf_reaches_the_client_as_evt_progress) {
    fresh();
    g_m.eval = deferring_eval;
    g_defer_token = 0;
    ASSERT_TRUE(post(71, "defer", NULL));
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 0);
    ASSERT_TRUE(g_defer_token != 0);
    ASSERT_EQ_INT(gs_result_request_id(g_defer_token), 71);
    gs_result_progress(g_defer_token, 5, 10);
    uint8_t buf[256];
    uint32_t len;
    ASSERT_EQ_INT(take_any(buf, sizeof buf, &len), GS_MBX_EVT_PROGRESS);
    uint32_t n = RD_LE32(buf + 4 * GS_MBX_EVENT_JSON_LEN);
    buf[4 * GS_MBX_EVENT_WORDS + n] = '\0';
    ASSERT_TRUE(strcmp((char *)buf + 4 * GS_MBX_EVENT_WORDS, "{\"id\":71,\"done\":5,\"total\":10}") == 0);
    gs_result_complete_ok(g_defer_token);
    uint32_t id, ok;
    char json[64];
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 71);
    // A token nobody holds names no request and reports nowhere.
    ASSERT_EQ_INT(gs_result_request_id(g_defer_token), 0);
    gs_result_progress(g_defer_token, 1, 1);
    ASSERT_EQ_INT(take_any(buf, sizeof buf, &len), 0);
}

TEST(a_staged_buffer_is_named_looked_up_and_freed_by_the_ack) {
    fresh();
    char *buf = (char *)malloc(64);
    strcpy(buf, "spilled");
    uint32_t h = gs_staged_publish(buf, 8, 0);
    ASSERT_TRUE(h != 0);
    void *ptr = NULL;
    size_t len = 0;
    ASSERT_TRUE(gs_staged_lookup(h, &ptr, &len));
    ASSERT_TRUE(ptr == buf && len == 8);
    // The client's ack frees it and is answered true; a second ack of the
    // same handle finds nothing.
    post_ctl(GS_MBX_REQ_ACK_BUF, 81, 1, h);
    post_ctl(GS_MBX_REQ_ACK_BUF, 82, 1, h);
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 2);
    uint32_t id, ok;
    char json[64];
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 81);
    ASSERT_TRUE(strcmp(json, "true") == 0);
    ASSERT_EQ_INT(take(&id, &ok, json, sizeof json), 1);
    ASSERT_EQ_INT(id, 82);
    ASSERT_TRUE(strcmp(json, "false") == 0);
    ASSERT_TRUE(!gs_staged_lookup(h, NULL, NULL));
    // The table is bounded; publishing past it fails, releasing makes room.
    uint32_t hs[GS_MBX_STAGED_MAX + 1];
    for (int i = 0; i < GS_MBX_STAGED_MAX; i++) {
        hs[i] = gs_staged_publish(malloc(4), 4, 0);
        ASSERT_TRUE(hs[i] != 0);
    }
    ASSERT_EQ_INT(gs_staged_publish(buf, 1, 0), 0);
    for (int i = 0; i < GS_MBX_STAGED_MAX; i++)
        gs_staged_release(hs[i]);
}

// An I/O job that runs until cancelled, on a real worker thread.
static int waiting_work(void *ud, char *err, size_t cap) {
    (void)ud;
    for (int i = 0; i < 50000; i++) {
        if (io_cancelled()) {
            snprintf(err, cap, "cancelled");
            return -ECANCELED;
        }
        io_progress((uint64_t)i, 50000);
        usleep(100);
    }
    return 0;
}
static void waiting_done(bool ok, double ms, const char *error, void *ud) {
    (void)ms;
    uint32_t token = *(uint32_t *)ud;
    if (ok)
        gs_result_complete_ok(token);
    else
        gs_result_complete_error(token, error);
}
static uint32_t g_io_token;
static int io_eval(const char *path, const char *args, char *out, size_t out_size) {
    (void)args;
    if (strcmp(path, "io") == 0) {
        g_io_token = gs_result_defer();
        uint32_t id = io_submit_work(waiting_work, NULL, waiting_done, &g_io_token);
        ASSERT_TRUE(id != 0);
        gs_result_bind_io(g_io_token, id);
        snprintf(out, out_size, "true");
        return 0;
    }
    return stub_eval(path, args, out, out_size);
}

TEST(cancelling_a_request_cancels_the_io_job_answering_it) {
    fresh();
    ASSERT_TRUE(io_worker_start(0));
    g_m.eval = io_eval;
    ASSERT_TRUE(post(91, "io", NULL));
    ASSERT_EQ_INT(gs_mailbox_drain(&g_m, 0, NULL), 0); // deferred
    usleep(5000);
    // The client gives up on it: the cancel is answered true and the job
    // ends as cancelled, which is the request's answer.
    post_ctl(GS_MBX_REQ_CANCEL, 92, 1, 91);
    gs_mailbox_drain(&g_m, 0, NULL);
    uint32_t id = 0, ok = 1;
    char json[128];
    bool saw_cancel_answer = false, saw_job_answer = false;
    for (int i = 0; i < 20000 && !(saw_cancel_answer && saw_job_answer); i++) {
        uint8_t buf[256];
        uint32_t len, kind;
        while ((kind = take_any(buf, sizeof buf, &len)) != 0) {
            if (kind != GS_MBX_EVT_RESULT)
                continue; // progress along the way
            id = RD_LE32(buf + 4 * GS_MBX_RESULT_ID);
            ok = RD_LE32(buf + 4 * GS_MBX_RESULT_OK);
            uint32_t n = RD_LE32(buf + 4 * GS_MBX_RESULT_JSON_LEN);
            memcpy(json, buf + 4 * GS_MBX_RESULT_WORDS, n);
            json[n] = '\0';
            if (id == 92) {
                saw_cancel_answer = true;
                ASSERT_TRUE(strcmp(json, "true") == 0);
            } else if (id == 91) {
                saw_job_answer = true;
                ASSERT_EQ_INT(ok, 0);
                ASSERT_TRUE(strstr(json, "cancelled") != NULL);
            }
        }
        gs_mailbox_drain(&g_m, 0, NULL);
        usleep(100);
    }
    ASSERT_TRUE(saw_cancel_answer && saw_job_answer);
}

// A job that prints: its text arrives as EVT_LOG output records, in
// order, before its result.
int job_glue_run_source_printing(const char *src);
TEST(a_jobs_output_arrives_as_output_records_before_its_result) {
    fresh();
    ASSERT_TRUE(job_thread_start(256u << 10));
    ASSERT_TRUE(post_script(101, "printing"));
    uint32_t id = 0, ok = 0;
    char json[256];
    char outputs[512] = "";
    int results = 0;
    for (int i = 0; i < 20000 && !results; i++) {
        gs_mailbox_drain(&g_m, 0, NULL);
        uint8_t buf[512];
        uint32_t len, kind;
        while ((kind = take_any(buf, sizeof buf, &len)) != 0) {
            if (kind == GS_MBX_EVT_LOG) {
                uint32_t n = RD_LE32(buf + 4 * GS_MBX_EVENT_JSON_LEN);
                buf[4 * GS_MBX_EVENT_WORDS + n] = '\0';
                strncat(outputs, (char *)buf + 4 * GS_MBX_EVENT_WORDS, sizeof outputs - strlen(outputs) - 1);
                strncat(outputs, "|", sizeof outputs - strlen(outputs) - 1);
                ASSERT_EQ_INT(results, 0); // output before the result
            } else if (kind == GS_MBX_EVT_RESULT) {
                id = RD_LE32(buf + 4 * GS_MBX_RESULT_ID);
                ok = RD_LE32(buf + 4 * GS_MBX_RESULT_OK);
                uint32_t n = RD_LE32(buf + 4 * GS_MBX_RESULT_JSON_LEN);
                memcpy(json, buf + 4 * GS_MBX_RESULT_WORDS, n);
                json[n] = '\0';
                results++;
            }
        }
        usleep(100);
    }
    ASSERT_EQ_INT(results, 1);
    ASSERT_EQ_INT(id, 101);
    ASSERT_EQ_INT(ok, 1);
    ASSERT_TRUE(strstr(outputs, "{\"event\":\"output\",\"id\":101,\"client\":2,\"text\":\"one \\\"quoted\\\"\\n\"}") !=
                NULL);
    ASSERT_TRUE(strstr(outputs, "two\\n") != NULL);
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
    RUN(a_script_without_a_job_thread_runs_inline_and_answers_the_prompt);
    RUN(cancel_and_mode_stop_are_answered_and_stop_only_the_owner);
    RUN(the_in_process_client_posts_and_reads_like_the_page);
    RUN(a_deferred_leaf_answers_when_completed_not_when_served);
    RUN(a_job_thread_calls_the_emulator_through_the_drain_and_waits_for_its_mode);
    RUN(what_a_leaf_prints_travels_with_its_answer_when_captured);
    RUN(progress_of_a_deferred_leaf_reaches_the_client_as_evt_progress);
    RUN(a_staged_buffer_is_named_looked_up_and_freed_by_the_ack);
    RUN(cancelling_a_request_cancels_the_io_job_answering_it);
    RUN(a_jobs_output_arrives_as_output_records_before_its_result);
    return 0;
}
