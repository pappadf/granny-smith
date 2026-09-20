// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for laserwriter_transport_ring.c, the LaserWriter bridge's
// transport in the browser build (laserwriter_ring_protocol.h).
//
// The suite is the worker: it takes the control block from the attach hook,
// reads the bridge's records from the outbound ring the way the browser
// worker will (part 2B mirrors the protocol in TypeScript), and answers
// through the inbound ring.  The rings are built small (the Makefile
// overrides LWRING_OUT_BYTES / LWRING_IN_BYTES) so a few records wrap them,
// need a PAD before the ring's end, or find no room at all — the cases a
// byte ring of unwrapped records gets wrong.

#include "laserwriter_ring_protocol.h"
#include "laserwriter_transport.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// The simulated worker
// ============================================================================

// What the attach hook handed us, and the worker's ring cursors.
static volatile uint32_t *g_ctrl;
static uint8_t *g_out; // core -> worker
static uint8_t *g_in; // worker -> core
static uint32_t g_out_size, g_in_size;
static uint32_t g_out_rd; // bytes consumed from the outbound ring
static uint32_t g_in_wr; // bytes written to the inbound ring
static int g_attach_calls;
static int g_notify_calls;
static int g_pads_seen; // PAD records the worker skipped (outbound)
static int g_pads_written; // PAD records the worker wrote (inbound)
static int g_out_records; // every outbound record start the worker observed ...
static int g_out_misaligned; // ... and how many were not 8-aligned (must stay 0)
static int g_in_records; // likewise for the records the worker wrote inbound
static int g_in_misaligned;

// The platform hook, overriding the transport's weak default: the page
// would hand the worker the shared memory and this address.
void laserwriter_ring_attach_requested(uintptr_t ctrl_addr) {
    g_attach_calls++;
    g_ctrl = (volatile uint32_t *)ctrl_addr;
    ASSERT_EQ_INT(g_ctrl[LWRING_C_MAGIC], LWRING_MAGIC);
    ASSERT_EQ_INT(g_ctrl[LWRING_C_VERSION], LWRING_PROTOCOL_VERSION);
    g_out = (uint8_t *)ctrl_addr + g_ctrl[LWRING_C_OUT_OFF];
    g_in = (uint8_t *)ctrl_addr + g_ctrl[LWRING_C_IN_OFF];
    g_out_size = g_ctrl[LWRING_C_OUT_SIZE];
    g_in_size = g_ctrl[LWRING_C_IN_SIZE];
    g_ctrl[LWRING_C_STATUS] = LWRING_STATUS_ATTACHED;
}

// The wake hook: counted, nothing to wake here.
void laserwriter_ring_notify(volatile uint32_t *addr) {
    (void)addr;
    g_notify_calls++;
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Reads the next outbound record (skipping PADs); returns false when the
// ring is empty.  `payload` points into the ring, valid until the next read.
static bool worker_read(uint32_t *kind, const uint8_t **payload, uint32_t *payload_len) {
    for (;;) {
        uint32_t head = __atomic_load_n(&g_ctrl[LWRING_C_OUT_HEAD], __ATOMIC_ACQUIRE);
        if (head - g_out_rd < LWRING_HDR_BYTES)
            return false;
        uint32_t at = g_out_rd & (g_out_size - 1u);
        // The framing rule: every record (PADs included) starts 8-aligned,
        // so a PAD's 8-byte header always fits before the ring's end
        g_out_records++;
        if (at & 7u)
            g_out_misaligned++;
        ASSERT_TRUE(g_out_size - at >= LWRING_HDR_BYTES);
        uint32_t k = rd_u32(g_out + at);
        uint32_t len = rd_u32(g_out + at + 4);
        ASSERT_TRUE(len >= LWRING_HDR_BYTES);
        ASSERT_TRUE((len & 7u) == 0);
        ASSERT_TRUE(at + len <= g_out_size); // a record never wraps
        g_out_rd += len;
        __atomic_store_n(&g_ctrl[LWRING_C_OUT_TAIL], g_out_rd, __ATOMIC_SEQ_CST);
        if (k == LWRING_R_PAD) {
            g_pads_seen++;
            continue;
        }
        *kind = k;
        *payload = g_out + at + LWRING_HDR_BYTES;
        *payload_len = len - LWRING_HDR_BYTES;
        return true;
    }
}

// Writes one inbound record from `words` header words and up to four text
// fields (each padded to 4; the record total padded to 8), with a PAD first
// when it would cross the end.  Returns false when there is no room.
static bool worker_write(uint32_t kind, const uint32_t *words, int n_words, const uint8_t *const *texts,
                         const uint32_t *text_lens, int n_texts) {
    uint32_t body = 4u * (uint32_t)n_words;
    for (int i = 0; i < n_texts; i++)
        body += LWRING_PAD4(text_lens[i]);
    uint32_t len = LWRING_PAD8(LWRING_HDR_BYTES + body);
    uint32_t mask = g_in_size - 1u;
    uint32_t at = g_in_wr & mask;
    g_in_records++;
    if (at & 7u)
        g_in_misaligned++;
    ASSERT_TRUE(g_in_size - at >= LWRING_HDR_BYTES); // room for at least a PAD header
    uint32_t pad = (at + len > g_in_size) ? g_in_size - at : 0;
    uint32_t tail = __atomic_load_n(&g_ctrl[LWRING_C_IN_TAIL], __ATOMIC_ACQUIRE);
    if (g_in_size - (g_in_wr - tail) < pad + len)
        return false;
    if (pad) {
        wr_u32(g_in + at, LWRING_R_PAD);
        wr_u32(g_in + at + 4, pad);
        g_in_wr += pad;
        at = 0;
        g_pads_written++;
        g_in_records++; // the real record starts at 0 after the PAD
    }
    wr_u32(g_in + at, kind);
    wr_u32(g_in + at + 4, len);
    uint8_t *p = g_in + at + LWRING_HDR_BYTES;
    for (int i = 0; i < n_words; i++)
        wr_u32(p + 4 * i, words[i]);
    p += 4 * n_words;
    for (int i = 0; i < n_texts; i++) {
        memcpy(p, texts[i], text_lens[i]);
        p += LWRING_PAD4(text_lens[i]);
    }
    g_in_wr += len;
    __atomic_store_n(&g_ctrl[LWRING_C_IN_HEAD], g_in_wr, __ATOMIC_SEQ_CST);
    return true;
}

static bool worker_send_opened(uint32_t job) {
    uint32_t w[LWRING_OPENED_WORDS] = {job};
    return worker_write(LWRING_R_OPENED, w, LWRING_OPENED_WORDS, NULL, NULL, 0);
}

static bool worker_send_open_failed(uint32_t job, const char *text) {
    uint32_t w[LWRING_OPEN_FAILED_WORDS] = {job, (uint32_t)strlen(text)};
    const uint8_t *t[1] = {(const uint8_t *)text};
    uint32_t l[1] = {(uint32_t)strlen(text)};
    return worker_write(LWRING_R_OPEN_FAILED, w, LWRING_OPEN_FAILED_WORDS, t, l, 1);
}

static bool worker_send_fed(uint32_t job, uint32_t seq, uint32_t status, uint32_t pages, const uint8_t *reply,
                            uint32_t reply_len, const uint8_t *errors, uint32_t error_len, uint32_t flags) {
    uint32_t w[LWRING_FED_WORDS];
    w[LWRING_FED_JOB] = job;
    w[LWRING_FED_SEQ] = seq;
    w[LWRING_FED_STATUS] = status;
    w[LWRING_FED_PAGES] = pages;
    w[LWRING_FED_REPLY_LEN] = reply_len;
    w[LWRING_FED_ERROR_LEN] = error_len;
    w[LWRING_FED_FLAGS] = flags;
    const uint8_t *t[2] = {reply, errors};
    uint32_t l[2] = {reply_len, error_len};
    return worker_write(LWRING_R_FED, w, LWRING_FED_WORDS, t, l, 2);
}

static bool worker_send_finished(uint32_t job, uint32_t outcome, uint32_t pages, const char *name, const char *offend,
                                 const uint8_t *reply, uint32_t reply_len) {
    uint32_t w[LWRING_FINISHED_WORDS];
    w[LWRING_FINISHED_JOB] = job;
    w[LWRING_FINISHED_OUTCOME] = outcome;
    w[LWRING_FINISHED_PAGES] = pages;
    w[LWRING_FINISHED_ERRNAME_LEN] = (uint32_t)strlen(name);
    w[LWRING_FINISHED_OFFEND_LEN] = (uint32_t)strlen(offend);
    w[LWRING_FINISHED_REPLY_LEN] = reply_len;
    w[LWRING_FINISHED_ERROR_LEN] = 0;
    w[LWRING_FINISHED_FLAGS] = 0;
    const uint8_t *t[4] = {(const uint8_t *)name, (const uint8_t *)offend, reply, (const uint8_t *)""};
    uint32_t l[4] = {(uint32_t)strlen(name), (uint32_t)strlen(offend), reply_len, 0};
    return worker_write(LWRING_R_FINISHED, w, LWRING_FINISHED_WORDS, t, l, 4);
}

// ============================================================================
// The bridge side: callbacks record what arrived
// ============================================================================

typedef struct {
    int opened, open_failed, fed, finished;
    uint32_t job_id;
    uint32_t seq;
    laserwriter_feed_status_t status;
    uint32_t pages;
    bool truncated;
    char text[256];
    uint8_t reply[4096];
    size_t reply_len;
    uint8_t errors[256];
    size_t errors_len;
    laserwriter_outcome_t outcome;
    bool pdf_present;
} events_t;

static events_t g_ev;

static void on_opened(uint32_t job_id, void *ctx) {
    ASSERT_TRUE(ctx == &g_ev);
    g_ev.opened++;
    g_ev.job_id = job_id;
}

static void on_open_failed(uint32_t job_id, const char *error, void *ctx) {
    ASSERT_TRUE(ctx == &g_ev);
    g_ev.open_failed++;
    g_ev.job_id = job_id;
    snprintf(g_ev.text, sizeof(g_ev.text), "%s", error);
}

static void on_fed(uint32_t job_id, uint32_t sequence, laserwriter_feed_status_t status, uint32_t pages,
                   const uint8_t *reply, size_t reply_len, const uint8_t *errors, size_t errors_len, bool truncated,
                   void *ctx) {
    ASSERT_TRUE(ctx == &g_ev);
    g_ev.fed++;
    g_ev.job_id = job_id;
    g_ev.seq = sequence;
    g_ev.status = status;
    g_ev.pages = pages;
    g_ev.truncated = truncated;
    ASSERT_TRUE(reply_len <= sizeof(g_ev.reply));
    ASSERT_TRUE(errors_len <= sizeof(g_ev.errors));
    memcpy(g_ev.reply, reply, reply_len);
    g_ev.reply_len = reply_len;
    memcpy(g_ev.errors, errors, errors_len);
    g_ev.errors_len = errors_len;
}

static void on_finished(uint32_t job_id, const laserwriter_finish_result_t *res, void *ctx) {
    ASSERT_TRUE(ctx == &g_ev);
    g_ev.finished++;
    g_ev.job_id = job_id;
    g_ev.outcome = res->outcome;
    g_ev.pages = res->pages;
    g_ev.pdf_present = res->pdf != NULL;
    snprintf(g_ev.text, sizeof(g_ev.text), "%s|%s", res->error_name, res->offending);
    ASSERT_TRUE(res->reply_len <= sizeof(g_ev.reply));
    memcpy(g_ev.reply, res->reply, res->reply_len);
    g_ev.reply_len = res->reply_len;
}

static const laserwriter_transport_callbacks_t g_callbacks = {
    .on_opened = on_opened,
    .on_open_failed = on_open_failed,
    .on_fed = on_fed,
    .on_finished = on_finished,
};

// A small configuration standing in for the bridge's identity and prelude.
static const laserwriter_identity_t g_identity[] = {
    {"product",  "(Test Press)"},
    {"revision", "3"           },
};
static const char g_prelude[] = "/lw_test 1 def\n";

static void config(laserwriter_job_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->identity = g_identity;
    cfg->identity_len = 2;
    cfg->prelude = (const uint8_t *)g_prelude;
    cfg->prelude_len = sizeof(g_prelude) - 1;
    cfg->server_password = -7;
    cfg->compress = true;
    cfg->embed_all_fonts = false;
    cfg->step_budget = 0x100000002ull; // both halves non-zero
}

// Opens `job` and has the worker answer OPENED: the common preamble.
static void open_and_ack(uint32_t job) {
    laserwriter_job_config_t cfg;
    config(&cfg);
    ASSERT_TRUE(laserwriter_transport_open(job, &cfg));
    uint32_t kind, len;
    const uint8_t *p;
    ASSERT_TRUE(worker_read(&kind, &p, &len));
    ASSERT_EQ_INT(kind, LWRING_R_OPEN);
    ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_OPEN_JOB), job);
    ASSERT_TRUE(worker_send_opened(job));
    int before = g_ev.opened;
    laserwriter_transport_poll();
    ASSERT_EQ_INT(g_ev.opened, before + 1);
    ASSERT_EQ_INT(g_ev.job_id, job);
}

// ============================================================================
// Tests
// ============================================================================

// ---- the records, field by field ------------------------------------------

TEST(open_carries_the_configuration) {
    laserwriter_job_config_t cfg;
    config(&cfg);
    ASSERT_TRUE(laserwriter_transport_open(11, &cfg));
    ASSERT_EQ_INT(g_attach_calls, 1); // the region exists and the platform was asked once
    ASSERT_TRUE(g_notify_calls > 0); // the worker was woken on OUT_HEAD

    uint32_t kind, len;
    const uint8_t *p;
    ASSERT_TRUE(worker_read(&kind, &p, &len));
    ASSERT_EQ_INT(kind, LWRING_R_OPEN);
    ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_OPEN_JOB), 11);
    ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_OPEN_COMPRESS), 1);
    ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_OPEN_EMBED), 0);
    ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_OPEN_BUDGET_L), 2);
    ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_OPEN_BUDGET_H), 1);
    ASSERT_EQ_INT((int32_t)rd_u32(p + 4 * LWRING_OPEN_PASSWORD), -7);
    ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_OPEN_ID_COUNT), 2);
    uint32_t id_bytes = rd_u32(p + 4 * LWRING_OPEN_ID_BYTES);
    ASSERT_EQ_INT(id_bytes, strlen("product") + 1 + strlen("(Test Press)") + 1 + strlen("revision") + 1 + 2);
    ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_OPEN_PRELUDE), sizeof(g_prelude) - 1);
    const char *q = (const char *)p + 4 * LWRING_OPEN_WORDS;
    ASSERT_TRUE(strcmp(q, "product") == 0);
    q += strlen(q) + 1;
    ASSERT_TRUE(strcmp(q, "(Test Press)") == 0);
    q += strlen(q) + 1;
    ASSERT_TRUE(strcmp(q, "revision") == 0);
    q += strlen(q) + 1;
    ASSERT_TRUE(strcmp(q, "3") == 0);
    q += strlen(q) + 1;
    ASSERT_TRUE(memcmp(q, g_prelude, sizeof(g_prelude) - 1) == 0);
    ASSERT_EQ_INT(len + LWRING_HDR_BYTES,
                  LWRING_PAD8(LWRING_HDR_BYTES + 4 * LWRING_OPEN_WORDS + id_bytes + sizeof(g_prelude) - 1));
    ASSERT_TRUE(!worker_read(&kind, &p, &len)); // nothing else

    // Nothing is delivered until the worker answers
    laserwriter_transport_poll();
    ASSERT_EQ_INT(g_ev.opened, 0);
    ASSERT_TRUE(worker_send_opened(11));
    laserwriter_transport_poll();
    ASSERT_EQ_INT(g_ev.opened, 1);
    ASSERT_EQ_INT(g_ev.job_id, 11);
}

TEST(feed_and_fed_round_trip) {
    // (job 11 is open from the previous test)
    uint8_t program[300];
    for (int i = 0; i < 300; i++)
        program[i] = (uint8_t)(i * 7);
    ASSERT_TRUE(laserwriter_transport_feed(11, 42, program, sizeof(program)));
    // One request at a time: a second feed is refused until FED
    ASSERT_TRUE(!laserwriter_transport_feed(11, 43, program, 10));

    uint32_t kind, len;
    const uint8_t *p;
    ASSERT_TRUE(worker_read(&kind, &p, &len));
    ASSERT_EQ_INT(kind, LWRING_R_FEED);
    ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_FEED_JOB), 11);
    ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_FEED_SEQ), 42);
    ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_FEED_LEN), 300);
    ASSERT_TRUE(memcmp(p + 4 * LWRING_FEED_WORDS, program, 300) == 0);

    const char *reply = "0\n";
    const char *errors = "%%[ Error: undefined; OffendingCommand: foo ]%%\n";
    ASSERT_TRUE(worker_send_fed(11, 42, LWRING_FEED_WAITING, 2, (const uint8_t *)reply, 2, (const uint8_t *)errors,
                                (uint32_t)strlen(errors), LWRING_FED_F_TRUNCATED));
    laserwriter_transport_poll();
    ASSERT_EQ_INT(g_ev.fed, 1);
    ASSERT_EQ_INT(g_ev.job_id, 11);
    ASSERT_EQ_INT(g_ev.seq, 42);
    ASSERT_EQ_INT(g_ev.status, LASERWRITER_FEED_WAITING);
    ASSERT_EQ_INT(g_ev.pages, 2);
    ASSERT_TRUE(g_ev.truncated);
    ASSERT_EQ_INT(g_ev.reply_len, 2);
    ASSERT_TRUE(memcmp(g_ev.reply, reply, 2) == 0);
    ASSERT_EQ_INT(g_ev.errors_len, strlen(errors));
    ASSERT_TRUE(memcmp(g_ev.errors, errors, strlen(errors)) == 0);

    // A feed larger than one flow quantum is refused outright
    uint8_t big[LWRING_FEED_MAX + 1];
    memset(big, 'x', sizeof(big));
    ASSERT_TRUE(!laserwriter_transport_feed(11, 44, big, sizeof(big)));
}

TEST(finish_and_finished_round_trip) {
    ASSERT_TRUE(laserwriter_transport_finish(11, "Macintosh HD"));
    uint32_t kind, len;
    const uint8_t *p;
    ASSERT_TRUE(worker_read(&kind, &p, &len));
    ASSERT_EQ_INT(kind, LWRING_R_FINISH);
    ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_FINISH_JOB), 11);
    // The title rides in FINISH (padded to 4; the record total to 8)
    ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_FINISH_TITLE_LEN), 12);
    ASSERT_TRUE(memcmp(p + 4 * LWRING_FINISH_WORDS, "Macintosh HD", 12) == 0);
    ASSERT_EQ_INT(len, LWRING_PAD8(4 * LWRING_FINISH_WORDS + 12));
    const char *tail = "done\n";
    ASSERT_TRUE(worker_send_finished(11, LWRING_OUTCOME_ERROR, 3, "limitcheck", "loop", (const uint8_t *)tail, 5));
    laserwriter_transport_poll();
    ASSERT_EQ_INT(g_ev.finished, 1);
    ASSERT_EQ_INT(g_ev.job_id, 11);
    ASSERT_EQ_INT(g_ev.outcome, LASERWRITER_OUTCOME_ERROR);
    ASSERT_EQ_INT(g_ev.pages, 3);
    ASSERT_TRUE(!g_ev.pdf_present); // the PDF never crosses the ring
    ASSERT_TRUE(strcmp(g_ev.text, "limitcheck|loop") == 0);
    ASSERT_EQ_INT(g_ev.reply_len, 5);
    ASSERT_TRUE(memcmp(g_ev.reply, tail, 5) == 0);
    // The job is gone: a feed for it is refused
    ASSERT_TRUE(!laserwriter_transport_feed(11, 45, (const uint8_t *)"x", 1));
}

TEST(open_failed_carries_the_reason) {
    laserwriter_job_config_t cfg;
    config(&cfg);
    ASSERT_TRUE(laserwriter_transport_open(12, &cfg));
    uint32_t kind, len;
    const uint8_t *p;
    ASSERT_TRUE(worker_read(&kind, &p, &len));
    ASSERT_EQ_INT(kind, LWRING_R_OPEN);
    ASSERT_TRUE(worker_send_open_failed(12, "prelude error: undefined in lw_test"));
    laserwriter_transport_poll();
    ASSERT_EQ_INT(g_ev.open_failed, 1);
    ASSERT_EQ_INT(g_ev.job_id, 12);
    ASSERT_TRUE(strcmp(g_ev.text, "prelude error: undefined in lw_test") == 0);
    // No job is held afterwards: opening again works
    open_and_ack(13);
}

// ---- abandon, stale answers, a lost worker --------------------------------

TEST(abandon_writes_the_record_and_drops_late_answers) {
    // (job 13 is open) Abandon it, then open 14; the worker, behind, still
    // answers for 13 first — that answer must not reach the bridge.
    laserwriter_transport_abandon(13);
    uint32_t kind, len;
    const uint8_t *p;
    ASSERT_TRUE(worker_read(&kind, &p, &len));
    ASSERT_EQ_INT(kind, LWRING_R_ABANDON);
    ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_ABANDON_JOB), 13);
    laserwriter_job_config_t cfg;
    config(&cfg);
    ASSERT_TRUE(laserwriter_transport_open(14, &cfg));
    ASSERT_TRUE(worker_read(&kind, &p, &len));
    ASSERT_EQ_INT(kind, LWRING_R_OPEN);
    ASSERT_TRUE(worker_send_fed(13, 1, LWRING_FEED_WAITING, 0, NULL, 0, NULL, 0, 0)); // stale
    ASSERT_TRUE(worker_send_opened(13)); // stale
    ASSERT_TRUE(worker_send_opened(14));
    int fed = g_ev.fed, opened = g_ev.opened;
    laserwriter_transport_poll();
    ASSERT_EQ_INT(g_ev.fed, fed);
    ASSERT_EQ_INT(g_ev.opened, opened + 1);
    ASSERT_EQ_INT(g_ev.job_id, 14);
    laserwriter_transport_abandon(14);
    ASSERT_TRUE(worker_read(&kind, &p, &len));
    ASSERT_EQ_INT(kind, LWRING_R_ABANDON);
    // Abandoning a job not held writes nothing
    laserwriter_transport_abandon(14);
    ASSERT_TRUE(!worker_read(&kind, &p, &len));
}

TEST(lost_worker_fails_the_outstanding_request) {
    open_and_ack(15);
    ASSERT_TRUE(laserwriter_transport_feed(15, 9, (const uint8_t *)"abc", 3));
    uint32_t kind, len;
    const uint8_t *p;
    ASSERT_TRUE(worker_read(&kind, &p, &len));
    ASSERT_EQ_INT(kind, LWRING_R_FEED);
    // The worker dies mid-feed
    __atomic_store_n(&g_ctrl[LWRING_C_STATUS], LWRING_STATUS_LOST, __ATOMIC_SEQ_CST);
    int fed = g_ev.fed;
    laserwriter_transport_poll();
    ASSERT_EQ_INT(g_ev.fed, fed + 1);
    ASSERT_EQ_INT(g_ev.job_id, 15);
    ASSERT_EQ_INT(g_ev.seq, 9);
    ASSERT_EQ_INT(g_ev.status, LASERWRITER_FEED_FAILED);
    // Only once: nothing is outstanding any more
    laserwriter_transport_poll();
    ASSERT_EQ_INT(g_ev.fed, fed + 1);
    // Recovered for the rest of the suite
    __atomic_store_n(&g_ctrl[LWRING_C_STATUS], LWRING_STATUS_ATTACHED, __ATOMIC_SEQ_CST);
    // The bridge dropped the job; the worker still gets no ABANDON (it is
    // gone) and a fresh open works
    ASSERT_TRUE(!worker_read(&kind, &p, &len));
}

// ---- wrap-around and PAD, both rings ---------------------------------------

TEST(outbound_ring_wraps_with_pads_and_keeps_every_byte) {
    open_and_ack(20);
    // 8 KB ring, 1000-byte feeds: the write cursor wraps every ~8 feeds and
    // most laps need a PAD (1008-byte records do not divide 8192).
    int pads_before = g_pads_seen;
    uint8_t program[1000];
    for (uint32_t seq = 1; seq <= 40; seq++) {
        for (int i = 0; i < 1000; i++)
            program[i] = (uint8_t)(seq * 31 + i);
        ASSERT_TRUE(laserwriter_transport_feed(20, seq, program, sizeof(program)));
        uint32_t kind, len;
        const uint8_t *p;
        ASSERT_TRUE(worker_read(&kind, &p, &len));
        ASSERT_EQ_INT(kind, LWRING_R_FEED);
        ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_FEED_SEQ), seq);
        ASSERT_EQ_INT(rd_u32(p + 4 * LWRING_FEED_LEN), 1000);
        ASSERT_TRUE(memcmp(p + 4 * LWRING_FEED_WORDS, program, 1000) == 0); // byte identity across the wrap
        ASSERT_TRUE(worker_send_fed(20, seq, LWRING_FEED_WAITING, 0, NULL, 0, NULL, 0, 0));
        laserwriter_transport_poll();
        ASSERT_EQ_INT(g_ev.seq, seq);
    }
    ASSERT_TRUE(g_out_rd > 4 * LWRING_OUT_BYTES); // the ring wrapped several times
    ASSERT_TRUE(g_pads_seen > pads_before); // and records needed PADs to avoid straddling the end
}

TEST(inbound_ring_wraps_with_pads_and_keeps_every_byte) {
    // (job 20 is open) 4 KB inbound ring, 1500-byte replies: every third
    // record or so needs a PAD, and the ring wraps many times.
    int pads_before = g_pads_written;
    uint8_t reply[1500];
    for (uint32_t seq = 100; seq < 140; seq++) {
        ASSERT_TRUE(laserwriter_transport_feed(20, seq, (const uint8_t *)"x", 1));
        uint32_t kind, len;
        const uint8_t *p;
        ASSERT_TRUE(worker_read(&kind, &p, &len));
        ASSERT_EQ_INT(kind, LWRING_R_FEED);
        for (int i = 0; i < 1500; i++)
            reply[i] = (uint8_t)(seq * 13 + i * 3);
        ASSERT_TRUE(
            worker_send_fed(20, seq, LWRING_FEED_WAITING, seq - 99, reply, sizeof(reply), (const uint8_t *)"e", 1, 0));
        laserwriter_transport_poll();
        ASSERT_EQ_INT(g_ev.seq, seq);
        ASSERT_EQ_INT(g_ev.pages, seq - 99);
        ASSERT_EQ_INT(g_ev.reply_len, 1500);
        ASSERT_TRUE(memcmp(g_ev.reply, reply, 1500) == 0);
        ASSERT_EQ_INT(g_ev.errors_len, 1);
        ASSERT_EQ_INT(g_ev.errors[0], 'e');
        // The core consumed it: the worker's next write has the room back
        ASSERT_EQ_INT(__atomic_load_n(&g_ctrl[LWRING_C_IN_TAIL], __ATOMIC_ACQUIRE), g_in_wr);
    }
    ASSERT_TRUE(g_in_wr > 8 * LWRING_IN_BYTES);
    ASSERT_TRUE(g_pads_written > pads_before);
    laserwriter_transport_abandon(20);
    uint32_t kind, len;
    const uint8_t *p;
    ASSERT_TRUE(worker_read(&kind, &p, &len));
    ASSERT_EQ_INT(kind, LWRING_R_ABANDON);
}

TEST(inbound_record_larger_than_the_remaining_space_is_padded_past_the_end) {
    // Steer the inbound cursor to just short of the ring's end, then write
    // a FED whose record cannot fit before it: the worker must PAD and
    // start at 0, and the core must skip the PAD and read it whole.
    open_and_ack(21);
    uint32_t mask = g_in_size - 1u;
    uint32_t seq = 500;
    while ((g_in_wr & mask) < g_in_size - 200) {
        // Small FEDs (32-byte records) creep the cursor forward
        ASSERT_TRUE(laserwriter_transport_feed(21, seq, (const uint8_t *)"y", 1));
        uint32_t kind, len;
        const uint8_t *p;
        ASSERT_TRUE(worker_read(&kind, &p, &len));
        ASSERT_TRUE(worker_send_fed(21, seq, LWRING_FEED_WAITING, 0, NULL, 0, NULL, 0, 0));
        laserwriter_transport_poll();
        ASSERT_EQ_INT(g_ev.seq, seq);
        seq++;
    }
    uint32_t at = g_in_wr & mask;
    ASSERT_TRUE(g_in_size - at < 400 + LWRING_HDR_BYTES + 4 * LWRING_FED_WORDS); // a 400-byte reply cannot fit
    int pads = g_pads_written;
    uint8_t reply[400];
    for (int i = 0; i < 400; i++)
        reply[i] = (uint8_t)(i ^ 0x5A);
    ASSERT_TRUE(laserwriter_transport_feed(21, seq, (const uint8_t *)"z", 1));
    uint32_t kind, len;
    const uint8_t *p;
    ASSERT_TRUE(worker_read(&kind, &p, &len));
    ASSERT_TRUE(worker_send_fed(21, seq, LWRING_FEED_ENDED, 1, reply, 400, NULL, 0, 0));
    ASSERT_EQ_INT(g_pads_written, pads + 1);
    ASSERT_EQ_INT(g_in_wr & mask, LWRING_PAD8(LWRING_HDR_BYTES + 4 * LWRING_FED_WORDS + 400)); // restarted at 0
    laserwriter_transport_poll();
    ASSERT_EQ_INT(g_ev.seq, seq);
    ASSERT_EQ_INT(g_ev.status, LASERWRITER_FEED_ENDED);
    ASSERT_EQ_INT(g_ev.reply_len, 400);
    ASSERT_TRUE(memcmp(g_ev.reply, reply, 400) == 0);
    laserwriter_transport_abandon(21);
    ASSERT_TRUE(worker_read(&kind, &p, &len));
}

TEST(outbound_ring_with_no_room_refuses_the_request) {
    // A worker that never consumes: OPEN records (the prelude each) pile up
    // in the 8 KB ring until one does not fit.  abandon/open pairs keep the
    // bridge's one-request rule satisfied while the ring fills.
    uint32_t job = 30;
    int opened_ok = 0;
    laserwriter_job_config_t cfg;
    config(&cfg);
    // Pad the open with a prelude of ~1.5 KB so few records fill the ring
    static uint8_t prelude[1500];
    memset(prelude, '%', sizeof(prelude));
    cfg.prelude = prelude;
    cfg.prelude_len = sizeof(prelude);
    bool refused = false;
    for (int i = 0; i < 20 && !refused; i++) {
        if (laserwriter_transport_open(job, &cfg)) {
            opened_ok++;
            laserwriter_transport_abandon(job); // 16 bytes more, no ack needed
            job++;
        } else {
            refused = true;
        }
    }
    ASSERT_TRUE(refused);
    ASSERT_TRUE(opened_ok >= 3 && opened_ok <= 6); // ~1.5 KB each into 8 KB
    // The refusal left no job held: after the worker drains, an open works
    uint32_t kind, len;
    const uint8_t *p;
    int opens = 0, abandons = 0;
    while (worker_read(&kind, &p, &len)) {
        if (kind == LWRING_R_OPEN)
            opens++;
        else if (kind == LWRING_R_ABANDON)
            abandons++;
    }
    ASSERT_EQ_INT(opens, opened_ok);
    ASSERT_EQ_INT(abandons, opened_ok);
    open_and_ack(job);
    laserwriter_transport_abandon(job);
    ASSERT_TRUE(worker_read(&kind, &p, &len));
}

// ---- the 8-byte framing rule --------------------------------------------

// Walks the outbound write position to exactly `target` (mod ring size)
// with 1-byte feeds (24-byte records) and 5-byte feeds (32-byte records):
// the distance is 8r bytes, and r = 3a + 4b with b = r mod 3 -- take the
// 32-byte steps first, then the 24-byte ones.  Every step is acknowledged
// so the bridge's one-request rule holds.
static void walk_out_to(uint32_t job, uint32_t *seq, uint32_t target) {
    uint32_t mask = g_out_size - 1u;
    uint32_t r = ((target - (g_out_rd & mask)) & mask) / 8u;
    if (r < 8u)
        r += g_out_size / 8u; // go once around: small distances are not 3a + 4b
    while (r > 0) {
        size_t len = (r % 3u != 0) ? 5 : 1;
        r -= (len == 5) ? 4u : 3u;
        ASSERT_TRUE(laserwriter_transport_feed(job, *seq, (const uint8_t *)"abcde", len));
        uint32_t kind, plen;
        const uint8_t *p;
        ASSERT_TRUE(worker_read(&kind, &p, &plen));
        ASSERT_EQ_INT(plen + LWRING_HDR_BYTES, len == 5 ? 32 : 24);
        ASSERT_TRUE(worker_send_fed(job, *seq, LWRING_FEED_WAITING, 0, NULL, 0, NULL, 0, 0));
        laserwriter_transport_poll();
        ASSERT_EQ_INT(g_ev.seq, *seq);
        (*seq)++;
    }
    ASSERT_EQ_INT(g_out_rd & mask, target);
}

TEST(outbound_pad_of_exactly_eight_bytes_at_size_minus_eight) {
    // Bring the write position to out_size - 8: the next feed cannot fit,
    // and the PAD the bridge writes is a bare 8-byte header.  Under the old
    // 4-byte rule this position could also be out_size - 4, where no PAD
    // header fits at all.
    open_and_ack(50);
    uint32_t seq = 1;
    walk_out_to(50, &seq, g_out_size - 8u);
    int pads = g_pads_seen;
    uint8_t program[100];
    memset(program, 'p', sizeof(program));
    ASSERT_TRUE(laserwriter_transport_feed(50, seq, program, sizeof(program)));
    // The worker sees the PAD (8 bytes, len 8) and then the record at 0
    uint32_t at = g_out_rd & (g_out_size - 1u);
    ASSERT_EQ_INT(at, g_out_size - 8u);
    ASSERT_EQ_INT(rd_u32(g_out + at), LWRING_R_PAD);
    ASSERT_EQ_INT(rd_u32(g_out + at + 4), 8);
    uint32_t kind, len;
    const uint8_t *p;
    ASSERT_TRUE(worker_read(&kind, &p, &len));
    ASSERT_EQ_INT(g_pads_seen, pads + 1);
    ASSERT_EQ_INT(kind, LWRING_R_FEED);
    ASSERT_EQ_INT((g_out_rd - len - LWRING_HDR_BYTES) & (g_out_size - 1u), 0); // restarted at 0
    ASSERT_TRUE(memcmp(p + 4 * LWRING_FEED_WORDS, program, 100) == 0);
    ASSERT_TRUE(worker_send_fed(50, seq, LWRING_FEED_WAITING, 0, NULL, 0, NULL, 0, 0));
    laserwriter_transport_poll();
    ASSERT_EQ_INT(g_ev.seq, seq);
    laserwriter_transport_abandon(50);
    ASSERT_TRUE(worker_read(&kind, &p, &len));
}

TEST(no_record_can_end_four_bytes_short_of_the_ring_end) {
    // Under the old rule a FEED of 4 bytes was 8 + 12 + 4 = 24 bytes -- fine
    // -- but a FEED of 1 byte was 8 + 12 + PAD4(1) = 24 too, and an OPEN of
    // odd size (8 + 36 + id + prelude) could be 4 mod 8; enough of those
    // left the write position at size - 4.  Now every record total is a
    // multiple of 8, so the position is always a multiple of 8: drive a
    // mix of sizes through both rings and check every observed start.
    open_and_ack(51);
    int out_before = g_out_records, in_before = g_in_records;
    uint32_t seq = 1;
    uint8_t reply[700];
    memset(reply, 'r', sizeof(reply));
    for (int i = 0; i < 300; i++) {
        size_t len = (size_t)(i * 37 % 61) + 1; // 1..61, every residue mod 8
        ASSERT_TRUE(laserwriter_transport_feed(51, seq, reply, len));
        uint32_t kind, plen;
        const uint8_t *p;
        ASSERT_TRUE(worker_read(&kind, &p, &plen));
        ASSERT_EQ_INT((plen + LWRING_HDR_BYTES) & 7u, 0);
        uint32_t rlen = (uint32_t)(i * 53 % 97) + 1; // odd reply sizes inbound too
        ASSERT_TRUE(worker_send_fed(51, seq, LWRING_FEED_WAITING, 0, reply, rlen, reply, (uint32_t)(i % 5), 0));
        laserwriter_transport_poll();
        ASSERT_EQ_INT(g_ev.seq, seq);
        ASSERT_EQ_INT(g_ev.reply_len, rlen);
        seq++;
    }
    ASSERT_TRUE(g_out_records - out_before >= 300);
    ASSERT_TRUE(g_in_records - in_before >= 300);
    ASSERT_TRUE(g_out_rd > 2 * LWRING_OUT_BYTES); // both rings wrapped meanwhile
    ASSERT_TRUE(g_in_wr > 2 * LWRING_IN_BYTES);
    ASSERT_EQ_INT(g_out_misaligned, 0);
    ASSERT_EQ_INT(g_in_misaligned, 0);
    laserwriter_transport_abandon(51);
    uint32_t kind, len;
    const uint8_t *p;
    ASSERT_TRUE(worker_read(&kind, &p, &len));
}

TEST(corrupt_inbound_framing_marks_the_worker_lost) {
    open_and_ack(40);
    ASSERT_TRUE(laserwriter_transport_feed(40, 1, (const uint8_t *)"q", 1));
    uint32_t kind, len;
    const uint8_t *p;
    ASSERT_TRUE(worker_read(&kind, &p, &len));
    // A record whose length is not a multiple of 8: the core cannot trust
    // anything after it and fails the feed as a lost worker
    uint32_t at = g_in_wr & (g_in_size - 1u);
    ASSERT_TRUE(at + 16 <= g_in_size);
    wr_u32(g_in + at, LWRING_R_FED);
    wr_u32(g_in + at + 4, 12); // 4-aligned, not 8-aligned: rejected
    g_in_wr += 16;
    __atomic_store_n(&g_ctrl[LWRING_C_IN_HEAD], g_in_wr, __ATOMIC_SEQ_CST);
    int fed = g_ev.fed;
    laserwriter_transport_poll();
    ASSERT_EQ_INT(__atomic_load_n(&g_ctrl[LWRING_C_STATUS], __ATOMIC_ACQUIRE), LWRING_STATUS_LOST);
    ASSERT_EQ_INT(g_ev.fed, fed + 1);
    ASSERT_EQ_INT(g_ev.status, LASERWRITER_FEED_FAILED);
}

int main(void) {
    laserwriter_transport_set_callbacks(&g_callbacks, &g_ev);
    ASSERT_TRUE(strcmp(laserwriter_transport_name(), "ring") == 0);
    RUN(open_carries_the_configuration);
    RUN(feed_and_fed_round_trip);
    RUN(finish_and_finished_round_trip);
    RUN(open_failed_carries_the_reason);
    RUN(abandon_writes_the_record_and_drops_late_answers);
    RUN(lost_worker_fails_the_outstanding_request);
    RUN(outbound_ring_wraps_with_pads_and_keeps_every_byte);
    RUN(inbound_ring_wraps_with_pads_and_keeps_every_byte);
    RUN(inbound_record_larger_than_the_remaining_space_is_padded_past_the_end);
    RUN(outbound_ring_with_no_room_refuses_the_request);
    RUN(outbound_pad_of_exactly_eight_bytes_at_size_minus_eight);
    RUN(no_record_can_end_four_bytes_short_of_the_ring_end);
    RUN(corrupt_inbound_framing_marks_the_worker_lost);
    // The whole suite's records, both rings, on the 8-byte rule
    ASSERT_TRUE(g_out_records > 0 && g_in_records > 0);
    ASSERT_EQ_INT(g_out_misaligned, 0);
    ASSERT_EQ_INT(g_in_misaligned, 0);
    fprintf(stderr, "laserwriter_ring: all tests passed (%d outbound / %d inbound record starts, all 8-aligned)\n",
            g_out_records, g_in_records);
    return 0;
}
