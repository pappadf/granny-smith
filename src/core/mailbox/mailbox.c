// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mailbox.c -- see mailbox.h.

#include "mailbox.h"

#include "common.h"
#include "log.h"
#include "io/io_worker.h"
#include "job/job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("mailbox");

__attribute__((weak)) void mailbox_notify(volatile uint32_t *word) {
    (void)word;
}

// The I/O worker's completions wake the emulator thread the way a request
// does: on REQ_HEAD, where the idle wait parks.
static volatile uint32_t *g_wake_word;
// The mailbox serving a leaf right now (mailbox_result_defer looks here), and
// the one completions go to (there is one mailbox per process).
static mailbox_t *g_serving_mailbox;
static mailbox_t *g_mailbox_for_completion;
static void wake_on_req_head(void) {
    if (g_wake_word)
        mailbox_notify(g_wake_word);
}

// A quarter of the event ring (mailbox_set_record_max); the default holds
// before any mailbox exists.
static size_t g_record_max = 16u << 10;

void mailbox_set_record_max(size_t bytes) {
    __atomic_store_n(&g_record_max, bytes, __ATOMIC_RELEASE);
}

size_t mailbox_record_max(void) {
    return __atomic_load_n(&g_record_max, __ATOMIC_ACQUIRE);
}

size_t mailbox_region_bytes(uint32_t req_bytes, uint32_t evt_bytes) {
    return GS_MBX_ALIGN + GS_MBX_CTRL_WORDS * 4u + req_bytes + evt_bytes;
}

volatile uint32_t *mailbox_init(mailbox_t *m, void *region, uint32_t req_bytes, uint32_t evt_bytes,
                                mailbox_eval_fn eval) {
    memset(m, 0, sizeof(*m));
    m->out = (char *)malloc(GS_MBX_RESULT_OUT);
    m->args = (char *)malloc(GS_MBX_ARGS_MAX + 1u);
    m->outbuf = (char *)malloc(GS_MBX_OUTPUT_MAX + 4u);
    if (!m->out || !m->args || !m->outbuf) {
        mailbox_free(m);
        return NULL;
    }
    uint32_t ctrl_bytes = GS_MBX_CTRL_WORDS * 4u;
    uintptr_t base = ((uintptr_t)region + (GS_MBX_ALIGN - 1u)) & ~(uintptr_t)(GS_MBX_ALIGN - 1u);
    m->ctrl = (volatile uint32_t *)base;
    uint8_t *req = (uint8_t *)(base + ctrl_bytes);
    mbx_ring_init(&m->req, m->ctrl, GS_MBX_C_REQ_HEAD, GS_MBX_C_REQ_TAIL, req, req_bytes);
    mbx_ring_init(&m->evt, m->ctrl, GS_MBX_C_EVT_HEAD, GS_MBX_C_EVT_TAIL, req + req_bytes, evt_bytes);
    m->eval = eval;
    mailbox_set_record_max(evt_bytes / 4u);
    for (int i = 0; i < GS_MBX_CTRL_WORDS; i++)
        m->ctrl[i] = 0;
    m->ctrl[GS_MBX_C_MAGIC] = GS_MAILBOX_MAGIC;
    m->ctrl[GS_MBX_C_VERSION] = GS_MAILBOX_VERSION;
    m->ctrl[GS_MBX_C_REQ_OFF] = ctrl_bytes;
    m->ctrl[GS_MBX_C_REQ_SIZE] = req_bytes;
    m->ctrl[GS_MBX_C_EVT_OFF] = ctrl_bytes + req_bytes;
    m->ctrl[GS_MBX_C_EVT_SIZE] = evt_bytes;
    m->ctrl[GS_MBX_C_STATUS] = GS_MBX_STATUS_ATTACHED;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_wake_word = &m->ctrl[GS_MBX_C_REQ_HEAD];
    g_mailbox_for_completion = m;
    job_layer_set_wake_word(&m->ctrl[GS_MBX_C_REQ_HEAD]);
    io_worker_set_waker(wake_on_req_head);
    return m->ctrl;
}

void mailbox_free(mailbox_t *m) {
    free(m->out);
    free(m->args);
    free(m->outbuf);
    m->out = NULL;
    m->args = NULL;
    m->outbuf = NULL;
    if (g_mailbox_for_completion == m)
        g_mailbox_for_completion = NULL;
}

void mailbox_set_capture_output(mailbox_t *m, bool on) {
    m->capture_output = on;
}

uint32_t mailbox_serving_client(void) {
    mailbox_t *m = g_mailbox_for_completion;
    return m ? m->client : 0;
}

bool mailbox_output_append(const char *text, size_t len) {
    mailbox_t *m = g_serving_mailbox;
    if (!m || !m->serving || !m->capture_output)
        return false;
    if (m->outbuf_cut)
        return true;
    if (m->outbuf_len + len > GS_MBX_OUTPUT_MAX) {
        size_t room = GS_MBX_OUTPUT_MAX - m->outbuf_len;
        memcpy(m->outbuf + m->outbuf_len, text, room);
        m->outbuf_len += (uint32_t)room;
        memcpy(m->outbuf + m->outbuf_len, "...", 3);
        m->outbuf_len += 3;
        m->outbuf_cut = true;
        return true;
    }
    memcpy(m->outbuf + m->outbuf_len, text, len);
    m->outbuf_len += (uint32_t)len;
    return true;
}

// === Transfer buffers ==========================================================

// One published transfer buffer: its handle and the I/O job that owns it.
typedef struct {
    uint32_t handle;
    uint32_t io_job;
} transfer_t;
static transfer_t g_transfer[GS_MBX_TRANSFER_MAX];
static uint32_t g_transfer_seq;

uint32_t mailbox_transfer_publish(uint32_t io_job) {
    for (int i = 0; i < GS_MBX_TRANSFER_MAX; i++) {
        if (g_transfer[i].handle)
            continue;
        g_transfer_seq = (g_transfer_seq + 1) & 0x7fffffffu;
        if (!g_transfer_seq)
            g_transfer_seq = 1;
        g_transfer[i].handle = g_transfer_seq;
        g_transfer[i].io_job = io_job;
        return g_transfer_seq;
    }
    return 0;
}

// The ack: the buffer is handed back to its job (it refills and publishes
// again under the same handle).  False when no buffer has that handle.
static bool transfer_ack(uint32_t handle) {
    for (int i = 0; i < GS_MBX_TRANSFER_MAX; i++) {
        if (g_transfer[i].handle != handle || !handle)
            continue;
        io_worker_ack(g_transfer[i].io_job, handle);
        return true;
    }
    return false;
}

void mailbox_transfer_release(uint32_t handle) {
    for (int i = 0; i < GS_MBX_TRANSFER_MAX; i++) {
        if (g_transfer[i].handle != handle || !handle)
            continue;
        memset(&g_transfer[i], 0, sizeof g_transfer[i]);
        return;
    }
}

void mailbox_set_ready(mailbox_t *m) {
    mbx_store(m->ctrl, GS_MBX_C_READY, 1u);
    mailbox_notify(&m->ctrl[GS_MBX_C_READY]);
}

void mailbox_heartbeat(mailbox_t *m) {
    m->heartbeat++;
    __atomic_store_n(&m->ctrl[GS_MBX_C_HEARTBEAT], m->heartbeat, __ATOMIC_RELAXED);
}

bool mailbox_has_requests(const mailbox_t *m) {
    return mbx_load(m->ctrl, GS_MBX_C_REQ_HEAD) != m->req.rd || job_layer_has_work() || io_worker_has_work();
}

static void stat_add(mailbox_t *m, int word, uint32_t n) {
    __atomic_store_n(&m->ctrl[word], m->ctrl[word] + n, __ATOMIC_RELAXED);
}

bool mailbox_write_result(mailbox_t *m, uint32_t id, bool ok, const char *json, uint32_t json_len, const char *output,
                          uint32_t out_len) {
    if (!output)
        out_len = 0;
    uint32_t json_pad = (json_len + 3u) & ~3u;
    uint32_t len = MBX_HDR_BYTES + 4u * GS_MBX_RESULT_WORDS + json_pad + ((out_len + 3u) & ~3u);
    uint32_t at = mbx_reserve(&m->evt, GS_MBX_EVT_RESULT, len);
    if (at == UINT32_MAX)
        return false;
    uint8_t *p = mbx_payload(&m->evt, at);
    WR_LE32(p + 4 * GS_MBX_RESULT_ID, id);
    WR_LE32(p + 4 * GS_MBX_RESULT_OK, ok ? 1u : 0u);
    WR_LE32(p + 4 * GS_MBX_RESULT_JSON_LEN, json_len);
    WR_LE32(p + 4 * GS_MBX_RESULT_OUT_LEN, out_len);
    memcpy(p + 4 * GS_MBX_RESULT_WORDS, json, json_len);
    if (out_len)
        memcpy(p + 4 * GS_MBX_RESULT_WORDS + json_pad, output, out_len);
    stat_add(m, GS_MBX_C_STAT_EVENTS, 1);
    return true;
}

bool mailbox_write_progress(mailbox_t *m, uint32_t id, uint64_t done, uint64_t total) {
    char json[96];
    snprintf(json, sizeof json, "{\"id\":%u,\"done\":%llu,\"total\":%llu}", (unsigned)id, (unsigned long long)done,
             (unsigned long long)total);
    return mailbox_emit(m, GS_MBX_EVT_PROGRESS, json);
}

// Writes the held/built answer as an EVT_RESULT (with the output captured
// for it).  False when the event ring has no room: the answer stays held.
static bool write_result(mailbox_t *m) {
    if (!mailbox_write_result(m, m->out_id, m->out_ok != 0, m->out, m->out_len, m->outbuf, m->held_out_len))
        return false;
    m->held = false;
    m->held_out_len = 0;
    return true;
}

// The words every control request starts with; false (and an error
// answer) when the record is too short.
static bool ctl_words(mailbox_t *m, const uint8_t *p, uint32_t payload_len, uint32_t *client, uint32_t *arg) {
    m->out_id = payload_len >= 4 ? RD_LE32(p + 4 * GS_MBX_CTL_ID) : 0;
    m->out_ok = 0;
    if (payload_len < 4u * GS_MBX_CTL_WORDS) {
        m->out_len = (uint32_t)snprintf(m->out, GS_MBX_RESULT_MAX, "{\"error\":\"mailbox: short control request\"}");
        stat_add(m, GS_MBX_C_STAT_BAD, 1);
        return false;
    }
    *client = RD_LE32(p + 4 * GS_MBX_CTL_CLIENT);
    *arg = RD_LE32(p + 4 * GS_MBX_CTL_ARG);
    return true;
}

static void answer_bool(mailbox_t *m, bool b) {
    m->out_ok = 1;
    m->out_len = (uint32_t)snprintf(m->out, GS_MBX_RESULT_MAX, b ? "true" : "false");
}

// REQ_SCRIPT: queued for the job thread (its result comes from
// job_layer_service when the job ends), or run inline when there is no
// job thread.  Returns whether an answer was built now.
static bool serve_script(mailbox_t *m, const uint8_t *p, uint32_t payload_len) {
    m->out_id = payload_len >= 4 ? RD_LE32(p + 4 * GS_MBX_SCRIPT_ID) : 0;
    m->out_ok = 0;
    if (payload_len < 4u * GS_MBX_SCRIPT_WORDS) {
        m->out_len = (uint32_t)snprintf(m->out, GS_MBX_RESULT_MAX, "{\"error\":\"mailbox: short REQ_SCRIPT\"}");
        stat_add(m, GS_MBX_C_STAT_BAD, 1);
        return true;
    }
    uint32_t client = RD_LE32(p + 4 * GS_MBX_SCRIPT_CLIENT);
    uint32_t src_len = RD_LE32(p + 4 * GS_MBX_SCRIPT_SRC_LEN);
    if (src_len > GS_MBX_SCRIPT_MAX || 4u * GS_MBX_SCRIPT_WORDS + src_len > payload_len) {
        m->out_len = (uint32_t)snprintf(m->out, GS_MBX_RESULT_MAX,
                                        "{\"error\":\"mailbox: REQ_SCRIPT length exceeds the record\"}");
        stat_add(m, GS_MBX_C_STAT_BAD, 1);
        return true;
    }
    const char *src = (const char *)(p + 4 * GS_MBX_SCRIPT_WORDS);
    stat_add(m, GS_MBX_C_STAT_REQUESTS, 1);
    if (job_submit_script(client, m->out_id, src, src_len))
        return false;
    // Inline: the script runs here, now, as this client.
    char *copy = (char *)malloc(src_len + 1);
    if (!copy) {
        m->out_len = (uint32_t)snprintf(m->out, GS_MBX_RESULT_MAX, "{\"error\":\"out of memory\"}");
        return true;
    }
    memcpy(copy, src, src_len);
    copy[src_len] = '\0';
    m->client = client;
    int rc = job_glue_run_source(copy, true);
    m->client = 0;
    free(copy);
    if (rc == 0) {
        m->out_ok = m->eval("shell.prompt", NULL, m->out, GS_MBX_RESULT_OUT) == 0;
        m->out_len = (uint32_t)strlen(m->out);
    } else {
        m->out_len = (uint32_t)snprintf(m->out, GS_MBX_RESULT_MAX, "{\"error\":\"command failed\"}");
    }
    return true;
}

// Serves one REQ_EVAL: fills m->out / out_id / out_ok.  A malformed request
// answers with an error result rather than being dropped, so the client's
// promise settles.  Returns whether an answer exists now (false: deferred).
static bool serve_eval(mailbox_t *m, const uint8_t *p, uint32_t payload_len) {
    m->out_id = payload_len >= 4 ? RD_LE32(p + 4 * GS_MBX_EVAL_ID) : 0;
    m->out_ok = 0;
    if (payload_len < 4u * GS_MBX_EVAL_WORDS) {
        m->out_len = (uint32_t)snprintf(m->out, GS_MBX_RESULT_MAX, "{\"error\":\"mailbox: short REQ_EVAL\"}");
        stat_add(m, GS_MBX_C_STAT_BAD, 1);
        return true;
    }
    uint32_t path_len = RD_LE32(p + 4 * GS_MBX_EVAL_PATH_LEN);
    uint32_t args_len = RD_LE32(p + 4 * GS_MBX_EVAL_ARGS_LEN);
    uint32_t path_pad = (path_len + 3u) & ~3u;
    if (path_len > GS_MBX_PATH_MAX || args_len > GS_MBX_ARGS_MAX ||
        4u * GS_MBX_EVAL_WORDS + path_pad + ((args_len + 3u) & ~3u) > payload_len) {
        m->out_len = (uint32_t)snprintf(m->out, GS_MBX_RESULT_MAX,
                                        "{\"error\":\"mailbox: REQ_EVAL lengths exceed the record\"}");
        stat_add(m, GS_MBX_C_STAT_BAD, 1);
        return true;
    }
    const uint8_t *path = p + 4 * GS_MBX_EVAL_WORDS;
    memcpy(m->path, path, path_len);
    m->path[path_len] = '\0';
    memcpy(m->args, path + path_pad, args_len);
    m->args[args_len] = '\0';
    m->client = RD_LE32(p + 4 * GS_MBX_EVAL_CLIENT);
    m->serving = true;
    m->deferred = false;
    m->outbuf_len = 0;
    m->outbuf_cut = false;
    g_serving_mailbox = m;
    int rc = m->eval(m->path, args_len ? m->args : NULL, m->out, GS_MBX_RESULT_OUT);
    g_serving_mailbox = NULL;
    m->serving = false;
    m->client = 0;
    m->out_ok = rc == 0 ? 1 : 0;
    m->out_len = (uint32_t)strlen(m->out);
    m->held_out_len = m->outbuf_len;
    stat_add(m, GS_MBX_C_STAT_REQUESTS, 1);
    // A deferred leaf has no answer yet: the request is consumed, the
    // answer comes with mailbox_result_complete (what it printed before
    // deferring goes with the answer then).
    if (m->deferred)
        m->held_out_len = 0;
    return !m->deferred;
}

// The mailbox serving a leaf right now (mailbox_result_defer looks here).

uint32_t mailbox_result_defer(void) {
    mailbox_t *m = g_serving_mailbox;
    if (m && m->serving && !m->deferred) {
        if (m->n_defers >= GS_MBX_DEFER_MAX)
            return 0; // too many in flight: the leaf works now
        uint32_t token = ++m->defer_seq & 0x7fffffffu;
        if (!token)
            token = ++m->defer_seq & 0x7fffffffu;
        m->defers[m->n_defers].token = token;
        m->defers[m->n_defers].req_id = m->out_id;
        m->defers[m->n_defers].io_job = 0;
        m->n_defers++;
        m->deferred = true;
        return token;
    }
    // Not a request: perhaps a job's call through the seam.
    return job_call_defer();
}

void mailbox_result_complete(uint32_t token, bool ok, const char *json) {
    if (token & 0x80000000u) {
        job_call_complete(token, ok, json);
        return;
    }
    mailbox_t *m = g_mailbox_for_completion;
    if (!m)
        return;
    for (int i = 0; i < m->n_defers; i++) {
        if (m->defers[i].token != token)
            continue;
        uint32_t req_id = m->defers[i].req_id;
        m->defers[i] = m->defers[--m->n_defers];
        // A result is at most GS_MBX_RESULT_MAX - 1 bytes, like a drain's (the
        // held buffer is GS_MBX_RESULT_MAX with its NUL): a larger one is an
        // error naming its size, never cut short.
        size_t len = strlen(json);
        char too_big[128];
        if (len >= GS_MBX_RESULT_MAX) {
            snprintf(too_big, sizeof too_big, "{\"error\":\"result is %zu bytes, over the %u-byte result limit\"}", len,
                     (unsigned)GS_MBX_RESULT_MAX);
            json = too_big;
            ok = false;
            len = strlen(json);
        }
        // No room on the event ring is the one thing that can go wrong
        // here; the answer is then held like a drain's result.
        if (!mailbox_write_result(m, req_id, ok, json, (uint32_t)len, NULL, 0)) {
            m->out_id = req_id;
            m->out_ok = ok ? 1 : 0;
            memcpy(m->out, json, len + 1);
            m->out_len = (uint32_t)len;
            m->held = true;
            m->held_out_len = 0;
            stat_add(m, GS_MBX_C_STAT_STALLS, 1);
            return;
        }
        mbx_publish(&m->evt);
        mailbox_notify(&m->ctrl[GS_MBX_C_EVT_HEAD]);
        return;
    }
}

void mailbox_result_complete_ok(uint32_t token) {
    mailbox_result_complete(token, true, "true");
}

void mailbox_result_bind_io(uint32_t token, uint32_t io_job) {
    if (token & 0x80000000u) {
        job_call_bind_io(token, io_job);
        return;
    }
    mailbox_t *m = g_mailbox_for_completion;
    if (!m)
        return;
    for (int i = 0; i < m->n_defers; i++)
        if (m->defers[i].token == token)
            m->defers[i].io_job = io_job;
}

uint32_t mailbox_result_request_id(uint32_t token) {
    if (token & 0x80000000u)
        return job_call_request_id(token);
    mailbox_t *m = g_mailbox_for_completion;
    if (!m)
        return 0;
    for (int i = 0; i < m->n_defers; i++)
        if (m->defers[i].token == token)
            return m->defers[i].req_id;
    return 0;
}

void mailbox_result_progress(uint32_t token, uint64_t done, uint64_t total) {
    mailbox_t *m = g_mailbox_for_completion;
    uint32_t id = mailbox_result_request_id(token);
    if (m && id)
        mailbox_write_progress(m, id, done, total);
}

// REQ_CANCEL names a request: a script job of the client, or a request a
// deferred I/O job is answering.
static bool cancel_request(uint32_t client, uint32_t target) {
    if (job_cancel(client, target) > 0)
        return true;
    mailbox_t *m = g_mailbox_for_completion;
    if (!m)
        return false;
    for (int i = 0; i < m->n_defers; i++)
        if (m->defers[i].req_id == target && m->defers[i].io_job)
            return io_worker_cancel(m->defers[i].io_job);
    return false;
}

void mailbox_result_complete_error(uint32_t token, const char *message) {
    char buf[512];
    size_t o = (size_t)snprintf(buf, sizeof buf, "{\"error\":\"");
    for (const char *c = message ? message : "failed"; *c && o + 8 < sizeof buf; c++) {
        if (*c == '"' || *c == '\\') {
            buf[o++] = '\\';
            buf[o++] = *c;
        } else if ((unsigned char)*c < 0x20) {
            buf[o++] = ' ';
        } else {
            buf[o++] = *c;
        }
    }
    snprintf(buf + o, sizeof buf - o, "\"}");
    mailbox_result_complete(token, false, buf);
}

uint32_t mailbox_current_client(const mailbox_t *m) {
    return m->client;
}

bool mailbox_emit(mailbox_t *m, uint32_t kind, const char *json) {
    if (mbx_load(m->ctrl, GS_MBX_C_STATUS) == GS_MBX_STATUS_LOST)
        return false;
    uint32_t n = (uint32_t)strlen(json);
    uint32_t len = MBX_HDR_BYTES + 4u * GS_MBX_EVENT_WORDS + ((n + 3u) & ~3u);
    uint32_t at = mbx_reserve(&m->evt, kind, len);
    if (at == UINT32_MAX) {
        stat_add(m, GS_MBX_C_STAT_DROPPED, 1);
        return false;
    }
    uint8_t *p = mbx_payload(&m->evt, at);
    WR_LE32(p + 4 * GS_MBX_EVENT_JSON_LEN, n);
    memcpy(p + 4 * GS_MBX_EVENT_WORDS, json, n);
    stat_add(m, GS_MBX_C_STAT_EVENTS, 1);
    mbx_publish(&m->evt);
    mailbox_notify(&m->ctrl[GS_MBX_C_EVT_HEAD]);
    return true;
}

bool mailbox_emit_output(mailbox_t *m, const char *json) {
    if (mbx_load(m->ctrl, GS_MBX_C_STATUS) == GS_MBX_STATUS_LOST)
        return true; // nobody to deliver to: let the job drop it
    uint32_t n = (uint32_t)strlen(json);
    uint32_t len = MBX_HDR_BYTES + 4u * GS_MBX_EVENT_WORDS + ((n + 3u) & ~3u);
    uint32_t at = mbx_reserve(&m->evt, GS_MBX_EVT_LOG, len);
    if (at == UINT32_MAX)
        return false;
    uint8_t *p = mbx_payload(&m->evt, at);
    WR_LE32(p + 4 * GS_MBX_EVENT_JSON_LEN, n);
    memcpy(p + 4 * GS_MBX_EVENT_WORDS, json, n);
    stat_add(m, GS_MBX_C_STAT_EVENTS, 1);
    return true;
}

int mailbox_drain(mailbox_t *m, double budget_us, double (*now_us)(void)) {
    if (mbx_load(m->ctrl, GS_MBX_C_STATUS) == GS_MBX_STATUS_LOST)
        return 0;
    double t0 = now_us ? now_us() : 0.0;
    int written = 0;
    // First the answer a previous drain could not deliver.
    if (m->held) {
        if (!write_result(m))
            return 0;
        written++;
    }
    for (;;) {
        uint32_t head = mbx_load(m->ctrl, GS_MBX_C_REQ_HEAD);
        mbx_rec_t rec;
        int got = mbx_next(&m->req, head, &rec);
        if (got == 0)
            break;
        if (got < 0) {
            LOG(0, "mailbox: request ring corrupt at %u; mailbox lost", (unsigned)m->req.rd);
            mbx_store(m->ctrl, GS_MBX_C_STATUS, GS_MBX_STATUS_LOST);
            mbx_abandon(&m->req, head);
            mailbox_notify(&m->ctrl[GS_MBX_C_STATUS]);
            break;
        }
        if (rec.kind == MBX_R_PAD) {
            mbx_consume(&m->req, &rec);
            continue;
        }
        m->held_out_len = 0;
        bool answered = true;
        if (rec.kind == GS_MBX_REQ_EVAL) {
            answered = serve_eval(m, mbx_rec_payload(&m->req, &rec), rec.len - MBX_HDR_BYTES);
        } else if (rec.kind == GS_MBX_REQ_SCRIPT) {
            answered = serve_script(m, mbx_rec_payload(&m->req, &rec), rec.len - MBX_HDR_BYTES);
        } else if (rec.kind == GS_MBX_REQ_CANCEL) {
            uint32_t client, target;
            if (ctl_words(m, mbx_rec_payload(&m->req, &rec), rec.len - MBX_HDR_BYTES, &client, &target))
                answer_bool(m, cancel_request(client, target));
        } else if (rec.kind == GS_MBX_REQ_ACK_BUF) {
            uint32_t client, handle;
            if (ctl_words(m, mbx_rec_payload(&m->req, &rec), rec.len - MBX_HDR_BYTES, &client, &handle))
                answer_bool(m, transfer_ack(handle));
        } else if (rec.kind == GS_MBX_REQ_MODE_STOP) {
            uint32_t client, owner;
            if (ctl_words(m, mbx_rec_payload(&m->req, &rec), rec.len - MBX_HDR_BYTES, &client, &owner))
                answer_bool(m, job_glue_stop_modes(owner));
        } else {
            // A kind this build does not serve: answer with an error if the
            // record is long enough to carry an id, else ignore it.
            m->out_id = rec.len >= MBX_HDR_BYTES + 4u ? RD_LE32(mbx_rec_payload(&m->req, &rec)) : 0;
            m->out_ok = 0;
            m->out_len = (uint32_t)snprintf(m->out, GS_MBX_RESULT_MAX,
                                            "{\"error\":\"mailbox: unsupported request kind %u\"}", (unsigned)rec.kind);
            stat_add(m, GS_MBX_C_STAT_BAD, 1);
        }
        // The request is consumed once its answer exists (held or written),
        // so a client that fills the ring sees room again as soon as the
        // core has taken the request in.
        mbx_consume(&m->req, &rec);
        if (!answered)
            continue;
        if (!write_result(m)) {
            m->held = true;
            stat_add(m, GS_MBX_C_STAT_STALLS, 1);
            break;
        }
        m->held_out_len = 0;
        written++;
        if (now_us && budget_us > 0.0 && now_us() - t0 >= budget_us)
            break;
    }
    // The jobs: a call posted by the job thread, a job's result; and the
    // I/O worker's completions.
    written += job_layer_service(m);
    io_worker_service();
    if (written || mbx_unpublished(&m->evt)) {
        mbx_publish(&m->evt);
        mailbox_notify(&m->ctrl[GS_MBX_C_EVT_HEAD]);
        if (now_us) {
            double us = now_us() - t0;
            if (us > (double)m->ctrl[GS_MBX_C_STAT_DRAIN_US])
                __atomic_store_n(&m->ctrl[GS_MBX_C_STAT_DRAIN_US], (uint32_t)us, __ATOMIC_RELAXED);
        }
    }
    return written;
}

// === An in-process client =====================================================

void mailbox_client_init(mailbox_client_t *c, mailbox_t *m) {
    memset(c, 0, sizeof(*c));
    c->m = m;
    uint8_t *base = (uint8_t *)m->ctrl;
    mbx_ring_init(&c->req, m->ctrl, GS_MBX_C_REQ_HEAD, GS_MBX_C_REQ_TAIL, base + m->ctrl[GS_MBX_C_REQ_OFF],
                  m->ctrl[GS_MBX_C_REQ_SIZE]);
    mbx_ring_init(&c->evt, m->ctrl, GS_MBX_C_EVT_HEAD, GS_MBX_C_EVT_TAIL, base + m->ctrl[GS_MBX_C_EVT_OFF],
                  m->ctrl[GS_MBX_C_EVT_SIZE]);
    c->next_id = 1;
}

static uint32_t client_next_id(mailbox_client_t *c) {
    uint32_t id = c->next_id++;
    if (c->next_id > 0x7fffffffu)
        c->next_id = 1;
    return id;
}

uint32_t mailbox_client_script(mailbox_client_t *c, uint32_t client, const char *src, size_t len) {
    if (len > GS_MBX_SCRIPT_MAX)
        return 0;
    uint32_t n = (uint32_t)len;
    uint32_t at = mbx_reserve(&c->req, GS_MBX_REQ_SCRIPT, MBX_HDR_BYTES + 4u * GS_MBX_SCRIPT_WORDS + ((n + 3u) & ~3u));
    if (at == UINT32_MAX)
        return 0;
    uint32_t id = client_next_id(c);
    uint8_t *p = mbx_payload(&c->req, at);
    WR_LE32(p + 4 * GS_MBX_SCRIPT_ID, id);
    WR_LE32(p + 4 * GS_MBX_SCRIPT_CLIENT, client);
    WR_LE32(p + 4 * GS_MBX_SCRIPT_DEADLINE, 0);
    WR_LE32(p + 4 * GS_MBX_SCRIPT_SRC_LEN, n);
    memcpy(p + 4 * GS_MBX_SCRIPT_WORDS, src, n);
    mbx_publish(&c->req);
    return id;
}

static uint32_t client_ctl(mailbox_client_t *c, uint32_t kind, uint32_t client, uint32_t arg) {
    uint32_t at = mbx_reserve(&c->req, kind, MBX_HDR_BYTES + 4u * GS_MBX_CTL_WORDS);
    if (at == UINT32_MAX)
        return 0;
    uint32_t id = client_next_id(c);
    uint8_t *p = mbx_payload(&c->req, at);
    WR_LE32(p + 4 * GS_MBX_CTL_ID, id);
    WR_LE32(p + 4 * GS_MBX_CTL_CLIENT, client);
    WR_LE32(p + 4 * GS_MBX_CTL_ARG, arg);
    mbx_publish(&c->req);
    return id;
}

uint32_t mailbox_client_cancel(mailbox_client_t *c, uint32_t client, uint32_t target_id) {
    return client_ctl(c, GS_MBX_REQ_CANCEL, client, target_id);
}

uint32_t mailbox_client_mode_stop(mailbox_client_t *c, uint32_t client, uint32_t owner) {
    return client_ctl(c, GS_MBX_REQ_MODE_STOP, client, owner);
}

uint32_t mailbox_client_take(mailbox_client_t *c, uint8_t *buf, size_t cap, uint32_t *len) {
    for (;;) {
        uint32_t head = mbx_load(c->m->ctrl, GS_MBX_C_EVT_HEAD);
        mbx_rec_t rec;
        int got = mbx_next(&c->evt, head, &rec);
        if (got <= 0)
            return 0;
        if (rec.kind == MBX_R_PAD) {
            mbx_consume(&c->evt, &rec);
            continue;
        }
        *len = rec.len - MBX_HDR_BYTES;
        memcpy(buf, mbx_rec_payload(&c->evt, &rec), *len < cap ? *len : cap);
        mbx_consume(&c->evt, &rec);
        return rec.kind;
    }
}
