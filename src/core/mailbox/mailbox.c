// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mailbox.c -- see mailbox.h.

#include "mailbox.h"

#include "common.h"
#include "log.h"
#include "job/job.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("mailbox");

__attribute__((weak)) void gs_mailbox_notify(volatile uint32_t *word) {
    (void)word;
}

size_t gs_mailbox_region_bytes(uint32_t req_bytes, uint32_t evt_bytes) {
    return GS_MBX_ALIGN + GS_MBX_CTRL_WORDS * 4u + req_bytes + evt_bytes;
}

volatile uint32_t *gs_mailbox_init(gs_mailbox_t *m, void *region, uint32_t req_bytes, uint32_t evt_bytes,
                                   gs_mailbox_eval_fn eval) {
    memset(m, 0, sizeof(*m));
    m->out = (char *)malloc(GS_MBX_RESULT_MAX);
    m->args = (char *)malloc(GS_MBX_ARGS_MAX + 1u);
    if (!m->out || !m->args) {
        gs_mailbox_free(m);
        return NULL;
    }
    uint32_t ctrl_bytes = GS_MBX_CTRL_WORDS * 4u;
    uintptr_t base = ((uintptr_t)region + (GS_MBX_ALIGN - 1u)) & ~(uintptr_t)(GS_MBX_ALIGN - 1u);
    m->ctrl = (volatile uint32_t *)base;
    uint8_t *req = (uint8_t *)(base + ctrl_bytes);
    mbx_ring_init(&m->req, m->ctrl, GS_MBX_C_REQ_HEAD, GS_MBX_C_REQ_TAIL, req, req_bytes);
    mbx_ring_init(&m->evt, m->ctrl, GS_MBX_C_EVT_HEAD, GS_MBX_C_EVT_TAIL, req + req_bytes, evt_bytes);
    m->eval = eval;
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
    job_layer_set_wake_word(&m->ctrl[GS_MBX_C_REQ_HEAD]);
    return m->ctrl;
}

void gs_mailbox_free(gs_mailbox_t *m) {
    free(m->out);
    free(m->args);
    m->out = NULL;
    m->args = NULL;
}

void gs_mailbox_set_ready(gs_mailbox_t *m) {
    mbx_store(m->ctrl, GS_MBX_C_READY, 1u);
    gs_mailbox_notify(&m->ctrl[GS_MBX_C_READY]);
}

void gs_mailbox_heartbeat(gs_mailbox_t *m) {
    m->heartbeat++;
    __atomic_store_n(&m->ctrl[GS_MBX_C_HEARTBEAT], m->heartbeat, __ATOMIC_RELAXED);
}

bool gs_mailbox_has_requests(const gs_mailbox_t *m) {
    return mbx_load(m->ctrl, GS_MBX_C_REQ_HEAD) != m->req.rd || job_layer_has_work();
}

static void stat_add(gs_mailbox_t *m, int word, uint32_t n) {
    __atomic_store_n(&m->ctrl[word], m->ctrl[word] + n, __ATOMIC_RELAXED);
}

bool gs_mailbox_write_result(gs_mailbox_t *m, uint32_t id, bool ok, const char *json, uint32_t json_len) {
    uint32_t len = MBX_HDR_BYTES + 4u * GS_MBX_RESULT_WORDS + ((json_len + 3u) & ~3u);
    uint32_t at = mbx_reserve(&m->evt, GS_MBX_EVT_RESULT, len);
    if (at == UINT32_MAX)
        return false;
    uint8_t *p = mbx_payload(&m->evt, at);
    WR_LE32(p + 4 * GS_MBX_RESULT_ID, id);
    WR_LE32(p + 4 * GS_MBX_RESULT_OK, ok ? 1u : 0u);
    WR_LE32(p + 4 * GS_MBX_RESULT_JSON_LEN, json_len);
    memcpy(p + 4 * GS_MBX_RESULT_WORDS, json, json_len);
    stat_add(m, GS_MBX_C_STAT_EVENTS, 1);
    return true;
}

// Writes the held/built answer as an EVT_RESULT.  False when the event
// ring has no room: the answer stays held.
static bool write_result(gs_mailbox_t *m) {
    if (!gs_mailbox_write_result(m, m->out_id, m->out_ok != 0, m->out, m->out_len))
        return false;
    m->held = false;
    return true;
}

// The words every control request starts with; false (and an error
// answer) when the record is too short.
static bool ctl_words(gs_mailbox_t *m, const uint8_t *p, uint32_t payload_len, uint32_t *client, uint32_t *arg) {
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

static void answer_bool(gs_mailbox_t *m, bool b) {
    m->out_ok = 1;
    m->out_len = (uint32_t)snprintf(m->out, GS_MBX_RESULT_MAX, b ? "true" : "false");
}

// REQ_SCRIPT: queued for the job thread (its result comes from
// job_layer_service when the job ends), or run inline when there is no
// job thread.  Returns whether an answer was built now.
static bool serve_script(gs_mailbox_t *m, const uint8_t *p, uint32_t payload_len) {
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
        m->out_ok = m->eval("shell.prompt", NULL, m->out, GS_MBX_RESULT_MAX) == 0;
        m->out_len = (uint32_t)strlen(m->out);
    } else {
        m->out_len = (uint32_t)snprintf(m->out, GS_MBX_RESULT_MAX, "{\"error\":\"command failed\"}");
    }
    return true;
}

// Serves one REQ_EVAL: fills m->out / out_id / out_ok.  A malformed request
// answers with an error result rather than being dropped, so the client's
// promise settles.
static void serve_eval(gs_mailbox_t *m, const uint8_t *p, uint32_t payload_len) {
    m->out_id = payload_len >= 4 ? RD_LE32(p + 4 * GS_MBX_EVAL_ID) : 0;
    m->out_ok = 0;
    if (payload_len < 4u * GS_MBX_EVAL_WORDS) {
        m->out_len = (uint32_t)snprintf(m->out, GS_MBX_RESULT_MAX, "{\"error\":\"mailbox: short REQ_EVAL\"}");
        stat_add(m, GS_MBX_C_STAT_BAD, 1);
        return;
    }
    uint32_t path_len = RD_LE32(p + 4 * GS_MBX_EVAL_PATH_LEN);
    uint32_t args_len = RD_LE32(p + 4 * GS_MBX_EVAL_ARGS_LEN);
    uint32_t path_pad = (path_len + 3u) & ~3u;
    if (path_len > GS_MBX_PATH_MAX || args_len > GS_MBX_ARGS_MAX ||
        4u * GS_MBX_EVAL_WORDS + path_pad + ((args_len + 3u) & ~3u) > payload_len) {
        m->out_len = (uint32_t)snprintf(m->out, GS_MBX_RESULT_MAX,
                                        "{\"error\":\"mailbox: REQ_EVAL lengths exceed the record\"}");
        stat_add(m, GS_MBX_C_STAT_BAD, 1);
        return;
    }
    const uint8_t *path = p + 4 * GS_MBX_EVAL_WORDS;
    memcpy(m->path, path, path_len);
    m->path[path_len] = '\0';
    memcpy(m->args, path + path_pad, args_len);
    m->args[args_len] = '\0';
    m->client = RD_LE32(p + 4 * GS_MBX_EVAL_CLIENT);
    int rc = m->eval(m->path, args_len ? m->args : NULL, m->out, GS_MBX_RESULT_MAX);
    m->client = 0;
    m->out_ok = rc == 0 ? 1 : 0;
    m->out_len = (uint32_t)strlen(m->out);
    stat_add(m, GS_MBX_C_STAT_REQUESTS, 1);
}

uint32_t gs_mailbox_current_client(const gs_mailbox_t *m) {
    return m->client;
}

bool gs_mailbox_emit(gs_mailbox_t *m, uint32_t kind, const char *json) {
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
    gs_mailbox_notify(&m->ctrl[GS_MBX_C_EVT_HEAD]);
    return true;
}

int gs_mailbox_drain(gs_mailbox_t *m, double budget_us, double (*now_us)(void)) {
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
            gs_mailbox_notify(&m->ctrl[GS_MBX_C_STATUS]);
            break;
        }
        if (rec.kind == MBX_R_PAD) {
            mbx_consume(&m->req, &rec);
            continue;
        }
        bool answered = true;
        if (rec.kind == GS_MBX_REQ_EVAL) {
            serve_eval(m, mbx_rec_payload(&m->req, &rec), rec.len - MBX_HDR_BYTES);
        } else if (rec.kind == GS_MBX_REQ_SCRIPT) {
            answered = serve_script(m, mbx_rec_payload(&m->req, &rec), rec.len - MBX_HDR_BYTES);
        } else if (rec.kind == GS_MBX_REQ_CANCEL) {
            uint32_t client, target;
            if (ctl_words(m, mbx_rec_payload(&m->req, &rec), rec.len - MBX_HDR_BYTES, &client, &target))
                answer_bool(m, job_cancel(client, target) > 0);
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
        written++;
        if (now_us && budget_us > 0.0 && now_us() - t0 >= budget_us)
            break;
    }
    // The jobs: a call posted by the job thread, a job's result.
    written += job_layer_service(m);
    if (written) {
        mbx_publish(&m->evt);
        if (now_us) {
            double us = now_us() - t0;
            if (us > (double)m->ctrl[GS_MBX_C_STAT_DRAIN_US])
                __atomic_store_n(&m->ctrl[GS_MBX_C_STAT_DRAIN_US], (uint32_t)us, __ATOMIC_RELAXED);
        }
    }
    return written;
}
