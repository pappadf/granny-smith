// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// laserwriter_transport_direct.c
// The headless transport: EfterScript's platen library linked into this
// process, called on the emulation thread.  Every request is queued and
// executed from a scheduler event a little later in guest time, and its
// result is delivered from that event, so the bridge sees the same
// asynchronous shape the browser's worker gives it (laserwriter_transport.h).
// Without a scheduler (a unit test) the queued request runs from
// laserwriter_transport_poll() instead.  Built only with PLATEN=1.

#include "laserwriter_transport.h"

#include "appletalk_internal.h"
#include "log.h"
#include "scheduler.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("appletalk");

#if GS_PLATEN

#include "platen.h"

// ============================================================================
// Constants and Macros
// ============================================================================

// Guest time between a request and its answer: a worker's turnaround.
// Small against the PAP issue gap (10 ms), large against zero, so the
// bridge is never answered in the instant it asked.
#define LASERWRITER_DIRECT_DELAY_NS 1000000ull

// Longest reply or error piece pulled from the library per call.
#define LASERWRITER_DRAIN_CHUNK 4096

// Identity entries kept from the open configuration.
#define LASERWRITER_IDENTITY_MAX 8

// ============================================================================
// Type Definitions (Private)
// ============================================================================

// The request kinds that wait for the scheduler.
typedef enum { OP_NONE = 0, OP_OPEN, OP_FEED, OP_FINISH } direct_op_t;

// A growable byte buffer for one output channel.
typedef struct {
    uint8_t *bytes;
    size_t len;
    size_t cap;
} direct_buf_t;

// The transport: the library job, the one pending request, its copied
// arguments, and the buffers a result is delivered from.
typedef struct {
    laserwriter_transport_callbacks_t cb;
    void *cb_ctx;
    platen_job *job; // the library job, NULL between jobs
    uint32_t job_id; // the bridge's id for it
    direct_op_t pending; // the request waiting for the scheduler
    uint32_t pending_job_id;
    uint32_t pending_seq;
    uint8_t *pending_bytes; // FEED: a copy of the program bytes
    size_t pending_len;
    // OPEN: the configuration, copied
    laserwriter_job_config_t cfg;
    char *identity_text[LASERWRITER_IDENTITY_MAX * 2];
    laserwriter_identity_t identity[LASERWRITER_IDENTITY_MAX];
    uint8_t *prelude;
    // Output drained from the library for the result being delivered
    direct_buf_t reply;
    direct_buf_t errors;
    int gap_event_token;
    scheduler_t *registered_with; // the scheduler the event type is registered on
} direct_state_t;

static direct_state_t g_direct;

// ============================================================================
// Forward Declarations
// ============================================================================

static void direct_run_pending(void);

// ============================================================================
// Static Helpers
// ============================================================================

// Appends `len` bytes to `b`, growing it; drops (and logs) on allocation failure.
static void direct_buf_append(direct_buf_t *b, const uint8_t *bytes, size_t len) {
    if (len == 0)
        return;
    if (b->len + len > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + len)
            cap *= 2;
        uint8_t *grown = realloc(b->bytes, cap);
        if (!grown) {
            LOG(1, "laserwriter: dropping %zu output bytes (out of memory)", len);
            return;
        }
        b->bytes = grown;
        b->cap = cap;
    }
    memcpy(b->bytes + b->len, bytes, len);
    b->len += len;
}

// Pulls everything the library has written on both channels into the
// result buffers: the reply channel, then the error reports.
static void direct_drain(void) {
    uint8_t chunk[LASERWRITER_DRAIN_CHUNK];
    size_t n;
    g_direct.reply.len = 0;
    g_direct.errors.len = 0;
    while ((n = platen_job_read_replies(g_direct.job, chunk, sizeof(chunk))) > 0)
        direct_buf_append(&g_direct.reply, chunk, n);
    while ((n = platen_job_read_errors(g_direct.job, chunk, sizeof(chunk))) > 0)
        direct_buf_append(&g_direct.errors, chunk, n);
}

// Releases the copied open configuration.
static void direct_free_config(void) {
    for (size_t i = 0; i < LASERWRITER_IDENTITY_MAX * 2; i++) {
        free(g_direct.identity_text[i]);
        g_direct.identity_text[i] = NULL;
    }
    free(g_direct.prelude);
    g_direct.prelude = NULL;
    memset(&g_direct.cfg, 0, sizeof(g_direct.cfg));
}

// Releases the library job.
static void direct_free_job(void) {
    if (g_direct.job) {
        platen_job_free(g_direct.job);
        g_direct.job = NULL;
    }
    g_direct.job_id = 0;
}

// Forgets the pending request (its event, its copied bytes).
static void direct_clear_pending(void) {
    g_direct.pending = OP_NONE;
    free(g_direct.pending_bytes);
    g_direct.pending_bytes = NULL;
    g_direct.pending_len = 0;
}

// Scheduler event: run the pending request now.
static void direct_event_cb(void *source, uint64_t data) {
    (void)source;
    (void)data;
    direct_run_pending();
}

// Arms the delivery event on the stack's scheduler; false when there is
// none (poll runs the request instead).
static bool direct_arm(void) {
    scheduler_t *sched = atalk_scheduler();
    if (!sched)
        return false;
    if (g_direct.registered_with != sched) {
        // Idempotent: a machine rebuild hands out a new scheduler
        scheduler_new_event_type(sched, "laserwriter", &g_direct.gap_event_token, "direct_reply", &direct_event_cb);
        g_direct.registered_with = sched;
    }
    remove_event(sched, &direct_event_cb, &g_direct.gap_event_token);
    scheduler_new_cpu_event(sched, &direct_event_cb, &g_direct.gap_event_token, 0, 0, LASERWRITER_DIRECT_DELAY_NS);
    return true;
}

// Cancels a delivery event, if any.
static void direct_disarm(void) {
    scheduler_t *sched = atalk_scheduler();
    if (sched && g_direct.registered_with == sched)
        remove_event(sched, &direct_event_cb, &g_direct.gap_event_token);
}

// Executes OPEN: builds the platen_config from the copy and creates the job.
static void direct_run_open(void) {
    uint32_t job_id = g_direct.pending_job_id;
    platen_entry identity[LASERWRITER_IDENTITY_MAX];
    for (size_t i = 0; i < g_direct.cfg.identity_len; i++) {
        identity[i].key = g_direct.identity[i].key;
        identity[i].value = g_direct.identity[i].value;
    }
    platen_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.abi_version = PLATEN_ABI_VERSION;
    cfg.identity = identity;
    cfg.identity_len = g_direct.cfg.identity_len;
    cfg.prelude = g_direct.prelude;
    cfg.prelude_len = g_direct.cfg.prelude_len;
    cfg.server_password = g_direct.cfg.server_password;
    cfg.compress = g_direct.cfg.compress ? 1 : 0;
    cfg.embed_all_fonts = g_direct.cfg.embed_all_fonts ? 1 : 0;
    cfg.step_budget = g_direct.cfg.step_budget;
    direct_free_job();
    g_direct.job = platen_job_new(&cfg);
    direct_free_config();
    if (!g_direct.job) {
        const char *why = platen_last_error();
        LOG(1, "laserwriter: job %u: interpreter refused the configuration: %s", (unsigned)job_id, why);
        if (g_direct.cb.on_open_failed)
            g_direct.cb.on_open_failed(job_id, why, g_direct.cb_ctx);
        return;
    }
    g_direct.job_id = job_id;
    if (g_direct.cb.on_opened)
        g_direct.cb.on_opened(job_id, g_direct.cb_ctx);
}

// Executes FEED: the bytes run, both channels are drained, FED is delivered.
static void direct_run_feed(uint32_t job_id, uint32_t seq, const uint8_t *bytes, size_t len) {
    laserwriter_feed_status_t status = LASERWRITER_FEED_WAITING;
    int rc = platen_job_feed(g_direct.job, bytes, len);
    if (rc < 0) {
        LOG(1, "laserwriter: job %u feed failed (%d): %s", (unsigned)job_id, rc, platen_last_error());
        status = LASERWRITER_FEED_FAILED;
    } else if (rc == PLATEN_DONE) {
        LOG(3, "laserwriter: job %u ended before its data did; %zu bytes discarded", (unsigned)job_id, len);
        status = LASERWRITER_FEED_ENDED;
    }
    direct_drain();
    if (g_direct.cb.on_fed)
        g_direct.cb.on_fed(job_id, seq, status, platen_job_pages(g_direct.job), g_direct.reply.bytes,
                           g_direct.reply.len, g_direct.errors.bytes, g_direct.errors.len, false, g_direct.cb_ctx);
}

// Executes FINISH: the program runs to completion; FINISHED carries the
// outcome, the pages, and the document's bytes (valid for the callback).
static void direct_run_finish(void) {
    uint32_t job_id = g_direct.pending_job_id;
    int outcome = platen_job_finish(g_direct.job);
    direct_drain();
    laserwriter_finish_result_t res;
    memset(&res, 0, sizeof(res));
    res.pages = platen_job_pages(g_direct.job);
    res.error_name = platen_job_error_name(g_direct.job);
    res.offending = platen_job_offending(g_direct.job);
    res.reply = g_direct.reply.bytes;
    res.reply_len = g_direct.reply.len;
    res.errors = g_direct.errors.bytes;
    res.errors_len = g_direct.errors.len;
    if (outcome < 0) {
        // The document could not be closed or the job is poisoned: nothing to keep
        LOG(1, "laserwriter: job %u finish failed (%d): %s", (unsigned)job_id, outcome, platen_last_error());
        res.outcome = LASERWRITER_OUTCOME_FAILED;
        res.error_name = platen_last_error();
        res.offending = "";
    } else {
        res.pdf = platen_job_pdf(g_direct.job, &res.pdf_len);
        res.outcome = outcome == PLATEN_OUTCOME_OK       ? LASERWRITER_OUTCOME_OK
                      : outcome == PLATEN_OUTCOME_BUDGET ? LASERWRITER_OUTCOME_BUDGET
                                                         : LASERWRITER_OUTCOME_ERROR;
    }
    if (g_direct.cb.on_finished)
        g_direct.cb.on_finished(job_id, &res, g_direct.cb_ctx);
    // The bridge copied what it keeps; the job is over either way
    direct_free_job();
}

// Runs whatever request is pending and delivers its result.
static void direct_run_pending(void) {
    direct_op_t op = g_direct.pending;
    if (op == OP_NONE)
        return;
    // Cleared first: a callback may issue the next request
    if (op == OP_OPEN) {
        g_direct.pending = OP_NONE;
        direct_run_open();
        return;
    }
    if (!g_direct.job || g_direct.job_id != g_direct.pending_job_id) {
        // The job went away (abandoned) between the request and its turn
        LOG(3, "laserwriter: job %u: request dropped, job gone", (unsigned)g_direct.pending_job_id);
        direct_clear_pending();
        return;
    }
    if (op == OP_FEED) {
        // Take the bytes out of the slot: the callback may queue the next request
        uint8_t *bytes = g_direct.pending_bytes;
        size_t len = g_direct.pending_len;
        uint32_t job_id = g_direct.pending_job_id;
        uint32_t seq = g_direct.pending_seq;
        g_direct.pending_bytes = NULL;
        g_direct.pending_len = 0;
        g_direct.pending = OP_NONE;
        direct_run_feed(job_id, seq, bytes, len);
        free(bytes);
        return;
    }
    g_direct.pending = OP_NONE;
    direct_run_finish();
}

// Queues a request and arms its delivery; false when one is already waiting.
static bool direct_queue(direct_op_t op, uint32_t job_id) {
    if (g_direct.pending != OP_NONE) {
        LOG(1, "laserwriter: job %u: a request is already outstanding", (unsigned)job_id);
        return false;
    }
    g_direct.pending = op;
    g_direct.pending_job_id = job_id;
    direct_arm();
    return true;
}

// ============================================================================
// Operations (Public API)
// ============================================================================

void laserwriter_transport_set_callbacks(const laserwriter_transport_callbacks_t *callbacks, void *ctx) {
    if (callbacks)
        g_direct.cb = *callbacks;
    else
        memset(&g_direct.cb, 0, sizeof(g_direct.cb));
    g_direct.cb_ctx = ctx;
}

bool laserwriter_transport_open(uint32_t job_id, const laserwriter_job_config_t *cfg) {
    if (!cfg || cfg->identity_len > LASERWRITER_IDENTITY_MAX)
        return false;
    if (g_direct.pending != OP_NONE)
        return false;
    direct_free_config();
    g_direct.cfg = *cfg;
    // Copy the strings: the caller's are valid for this call only
    for (size_t i = 0; i < cfg->identity_len; i++) {
        g_direct.identity_text[2 * i] = strdup(cfg->identity[i].key ? cfg->identity[i].key : "");
        g_direct.identity_text[2 * i + 1] = strdup(cfg->identity[i].value ? cfg->identity[i].value : "");
        g_direct.identity[i].key = g_direct.identity_text[2 * i];
        g_direct.identity[i].value = g_direct.identity_text[2 * i + 1];
        if (!g_direct.identity[i].key || !g_direct.identity[i].value) {
            direct_free_config();
            return false;
        }
    }
    g_direct.cfg.identity = g_direct.identity;
    if (cfg->prelude_len) {
        g_direct.prelude = malloc(cfg->prelude_len);
        if (!g_direct.prelude) {
            direct_free_config();
            return false;
        }
        memcpy(g_direct.prelude, cfg->prelude, cfg->prelude_len);
    }
    g_direct.cfg.prelude = g_direct.prelude;
    LOG(3, "laserwriter: job %u open queued (direct)", (unsigned)job_id);
    return direct_queue(OP_OPEN, job_id);
}

bool laserwriter_transport_feed(uint32_t job_id, uint32_t sequence, const uint8_t *bytes, size_t len) {
    if (!g_direct.job || g_direct.job_id != job_id) {
        LOG(1, "laserwriter: job %u: feed with no such job", (unsigned)job_id);
        return false;
    }
    if (g_direct.pending != OP_NONE)
        return false;
    uint8_t *copy = NULL;
    if (len) {
        copy = malloc(len);
        if (!copy)
            return false;
        memcpy(copy, bytes, len);
    }
    g_direct.pending_bytes = copy;
    g_direct.pending_len = len;
    g_direct.pending_seq = sequence;
    return direct_queue(OP_FEED, job_id);
}

bool laserwriter_transport_finish(uint32_t job_id) {
    if (!g_direct.job || g_direct.job_id != job_id) {
        LOG(1, "laserwriter: job %u: finish with no such job", (unsigned)job_id);
        return false;
    }
    return direct_queue(OP_FINISH, job_id);
}

void laserwriter_transport_abandon(uint32_t job_id) {
    if (g_direct.pending != OP_NONE && g_direct.pending_job_id == job_id) {
        direct_disarm();
        direct_clear_pending();
        direct_free_config();
    }
    if (g_direct.job && g_direct.job_id == job_id)
        direct_free_job();
}

void laserwriter_transport_poll(void) {
    // With a scheduler the event delivers; without one, this does
    if (!atalk_scheduler())
        direct_run_pending();
}

const char *laserwriter_transport_name(void) {
    return "direct";
}

#else // !GS_PLATEN

// ============================================================================
// Stubs for builds without the interpreter
// ============================================================================

void laserwriter_transport_set_callbacks(const laserwriter_transport_callbacks_t *callbacks, void *ctx) {
    (void)callbacks;
    (void)ctx;
}

bool laserwriter_transport_open(uint32_t job_id, const laserwriter_job_config_t *cfg) {
    (void)job_id;
    (void)cfg;
    return false;
}

bool laserwriter_transport_feed(uint32_t job_id, uint32_t sequence, const uint8_t *bytes, size_t len) {
    (void)job_id;
    (void)sequence;
    (void)bytes;
    (void)len;
    return false;
}

bool laserwriter_transport_finish(uint32_t job_id) {
    (void)job_id;
    return false;
}

void laserwriter_transport_abandon(uint32_t job_id) {
    (void)job_id;
}

void laserwriter_transport_poll(void) {}

const char *laserwriter_transport_name(void) {
    return "none";
}

#endif // GS_PLATEN
