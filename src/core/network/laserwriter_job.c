// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// laserwriter_job.c
// One interpreter job per EOF-delimited PAP job, driven through the
// transport: begin issues OPEN, feed issues FEED, finish issues FINISH, and
// the transport's answers arrive later as callbacks that this module turns
// into the PAP layer's events.  The status text and the document hand-off
// live here too.  See laserwriter_job.h.

#include "laserwriter_job.h"

#include "appletalk_internal.h"
#include "laserwriter_transport.h"
#include "log.h"
#include "scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("appletalk");

// ============================================================================
// Platform sink default
// ============================================================================

// Fallback when no platform sink is linked: the document is lost, loudly.
__attribute__((weak)) void laserwriter_sink_document(const laserwriter_document_t *doc) {
    LOG(1, "laserwriter: job %u '%s' (%u pages, %zu bytes) dropped: no document sink on this platform",
        (unsigned)doc->job_id, doc->title, (unsigned)doc->pages, doc->pdf_len);
}

#if GS_PLATEN

#include "laserwriter_prelude.h"

// ============================================================================
// Constants and Macros
// ============================================================================

// Objects one job may execute before it is stopped.  The captured Finder
// job runs in under 10,000; a runaway `{ } loop` reaches this bound in ~7 s
// native (docs/notes/2026-09-16-efterscript-platen-bridge.md), which keeps a
// bad job from hanging a browser tab while leaving four orders of magnitude
// for real documents.
#define LASERWRITER_STEP_BUDGET 100000000ull

// Cap on unread program output; a real LaserWriter would block the program
// instead, which the ABI cannot.
#define LASERWRITER_OUTPUT_MAX (1u << 20)

// Guest time between polls of the transport while a request is outstanding
// (the ring transport delivers from the poll; the direct one from its own
// event, and the poll is then a no-op).
#define LASERWRITER_POLL_NS 1000000ull

// How long an OPEN may stay unanswered: in the browser the interpreter
// module is fetched on the first print job, so a first open waits for the
// download.  Past this the job fails and the session aborts.
#define LASERWRITER_OPEN_TIMEOUT_NS (120ull * 1000000000ull)

// ============================================================================
// Type Definitions (Private)
// ============================================================================

// Where the %%Title: scanner is.
typedef enum { TITLE_SEARCHING = 0, TITLE_COLLECTING, TITLE_DONE } title_scan_state_t;

// Where the job is with the transport.
typedef enum {
    JOB_IDLE = 0, // no job
    JOB_OPENING, // OPEN issued, OPENED not yet in
    JOB_READY, // opened; nothing outstanding
    JOB_FEEDING, // FEED issued
    JOB_FINISHING // FINISH issued
} job_state_t;

// The single job (a LaserWriter serves one connection at a time).
typedef struct {
    job_state_t state;
    uint32_t job_id;
    uint32_t pages; // as of the last acknowledgement
    bool ended_early; // a feed reported the job ended before its data did
    double open_deadline_ns; // scheduler time by which OPENED must arrive
    char title[LASERWRITER_TITLE_MAX + 1];
    title_scan_state_t title_state;
    uint8_t title_window_len; // bytes of the "%%Title:" pattern matched so far
    uint8_t *output; // program output not yet read by the PAP layer
    size_t output_len;
    size_t output_cap;
    size_t output_dropped; // bytes lost to LASERWRITER_OUTPUT_MAX
    uint32_t documents; // finished jobs handed to the sink
    uint32_t last_pages;
    char last_outcome[128];
    laserwriter_listener_t listener;
    void *listener_ctx;
    int poll_event_token;
    scheduler_t *poll_registered_with; // the scheduler the poll event type is registered on
} laserwriter_state_t;

static laserwriter_state_t g_lw;

// ============================================================================
// Forward Declarations
// ============================================================================

static void lw_poll_cb(void *source, uint64_t data);

// ============================================================================
// Static Helpers
// ============================================================================

// True while the transport owes an answer.
static bool lw_outstanding(void) {
    return g_lw.state == JOB_OPENING || g_lw.state == JOB_FEEDING || g_lw.state == JOB_FINISHING;
}

// Tells the PAP layer.
static void lw_notify(laserwriter_event_t event, const char *detail) {
    if (g_lw.listener)
        g_lw.listener(event, detail ? detail : "", g_lw.listener_ctx);
}

// Arms the transport poll tick for one period; re-armed from the tick
// while a request is outstanding.
static void lw_poll_arm(void) {
    scheduler_t *sched = atalk_scheduler();
    if (!sched)
        return;
    if (g_lw.poll_registered_with != sched) {
        // Idempotent: a machine rebuild hands out a new scheduler
        scheduler_new_event_type(sched, "laserwriter", &g_lw.poll_event_token, "transport_poll", &lw_poll_cb);
        g_lw.poll_registered_with = sched;
    }
    remove_event(sched, &lw_poll_cb, &g_lw.poll_event_token);
    scheduler_new_cpu_event(sched, &lw_poll_cb, &g_lw.poll_event_token, 0, 0, LASERWRITER_POLL_NS);
}

// Cancels the poll tick.
static void lw_poll_disarm(void) {
    scheduler_t *sched = atalk_scheduler();
    if (sched && g_lw.poll_registered_with == sched)
        remove_event(sched, &lw_poll_cb, &g_lw.poll_event_token);
}

// Appends `len` bytes to the unread-output buffer, dropping past the cap.
static void lw_output_append(const uint8_t *bytes, size_t len) {
    if (!bytes || len == 0)
        return;
    if (g_lw.output_len + len > LASERWRITER_OUTPUT_MAX) {
        g_lw.output_dropped += len;
        return;
    }
    if (g_lw.output_len + len > g_lw.output_cap) {
        size_t cap = g_lw.output_cap ? g_lw.output_cap : 4096;
        while (cap < g_lw.output_len + len)
            cap *= 2;
        uint8_t *grown = realloc(g_lw.output, cap);
        if (!grown) {
            g_lw.output_dropped += len;
            return;
        }
        g_lw.output = grown;
        g_lw.output_cap = cap;
    }
    memcpy(g_lw.output + g_lw.output_len, bytes, len);
    g_lw.output_len += len;
}

// Queues what a feed or a finish produced: the reply channel first, then
// the error reports, as the library ordered them.
static void lw_take_output(const uint8_t *reply, size_t reply_len, const uint8_t *errors, size_t errors_len) {
    if (reply_len)
        LOG(3, "laserwriter: job %u reply %zu bytes", (unsigned)g_lw.job_id, reply_len);
    lw_output_append(reply, reply_len);
    if (errors_len) {
        LOG(2, "laserwriter: job %u error report: %.*s", (unsigned)g_lw.job_id,
            (int)(errors_len > 200 ? 200 : errors_len), errors);
        lw_output_append(errors, errors_len);
    }
}

// Scans fed bytes for the driver's "%%Title: <name>" line and keeps the
// name; the pattern and the line may each span feeds.
static void lw_scan_title(const uint8_t *data, size_t len) {
    static const char pattern[] = "%%Title:";
    const size_t pattern_len = sizeof(pattern) - 1;
    if (g_lw.title_state == TITLE_DONE)
        return; // found in an earlier feed
    for (size_t i = 0; i < len && g_lw.title_state != TITLE_DONE; i++) {
        char c = (char)data[i];
        if (g_lw.title_state == TITLE_SEARCHING) {
            // Byte-at-a-time prefix match; a mismatch restarts on the same byte
            if (c == pattern[g_lw.title_window_len]) {
                if (++g_lw.title_window_len == pattern_len)
                    g_lw.title_state = TITLE_COLLECTING;
            } else {
                g_lw.title_window_len = (c == pattern[0]) ? 1 : 0;
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            g_lw.title_state = TITLE_DONE;
            break;
        }
        size_t n = strlen(g_lw.title);
        // Leading blanks and the driver's optional quoting are not part of the name
        if (n == 0 && (c == ' ' || c == '\t' || c == '(' || c == '"'))
            continue;
        if (n < LASERWRITER_TITLE_MAX) {
            g_lw.title[n] = c;
            g_lw.title[n + 1] = '\0';
        }
    }
    if (g_lw.title_state == TITLE_DONE) {
        // Strip a closing quote or parenthesis and trailing blanks
        size_t n = strlen(g_lw.title);
        while (n > 0 && (g_lw.title[n - 1] == ' ' || g_lw.title[n - 1] == ')' || g_lw.title[n - 1] == '"'))
            g_lw.title[--n] = '\0';
        LOG(2, "laserwriter: job %u title '%s'", (unsigned)g_lw.job_id, g_lw.title);
    }
}

// Forgets the job's per-job fields; output stays readable.
static void lw_release(void) {
    lw_poll_disarm();
    g_lw.state = JOB_IDLE;
    g_lw.pages = 0;
    g_lw.ended_early = false;
    g_lw.title[0] = '\0';
    g_lw.title_state = TITLE_SEARCHING;
    g_lw.title_window_len = 0;
}

// The job is unusable: drop it and tell the PAP layer why.
static void lw_fail(const char *why) {
    uint32_t job_id = g_lw.job_id;
    LOG(1, "laserwriter: job %u failed: %s", (unsigned)job_id, why);
    laserwriter_transport_abandon(job_id);
    snprintf(g_lw.last_outcome, sizeof(g_lw.last_outcome), "failed: %s", why);
    lw_release();
    lw_notify(LASERWRITER_EVENT_FAILED, why);
}

// True when a transport answer names the job we hold.
static bool lw_is_ours(uint32_t job_id, const char *what) {
    if (g_lw.state != JOB_IDLE && job_id == g_lw.job_id)
        return true;
    LOG(3, "laserwriter: %s for job %u ignored (holding job %u)", what, (unsigned)job_id,
        g_lw.state == JOB_IDLE ? 0u : (unsigned)g_lw.job_id);
    return false;
}

// The poll tick: drains the transport, checks the open deadline, and stays
// armed while an answer is owed.
static void lw_poll_cb(void *source, uint64_t data) {
    (void)source;
    (void)data;
    laserwriter_transport_poll();
    if (g_lw.state == JOB_OPENING) {
        scheduler_t *sched = atalk_scheduler();
        if (sched && scheduler_time_ns(sched) >= g_lw.open_deadline_ns) {
            lw_fail("the interpreter did not start");
            return;
        }
    }
    if (lw_outstanding())
        lw_poll_arm();
}

// ============================================================================
// Transport callbacks
// ============================================================================

// OPENED: the job takes data.
static void lw_on_opened(uint32_t job_id, void *ctx) {
    (void)ctx;
    if (!lw_is_ours(job_id, "OPENED") || g_lw.state != JOB_OPENING)
        return;
    g_lw.state = JOB_READY;
    LOG(2, "laserwriter: job %u started (%s)", (unsigned)job_id, laserwriter_transport_name());
    lw_notify(LASERWRITER_EVENT_OPENED, NULL);
}

// OPEN_FAILED: the configuration was refused or the interpreter is gone.
static void lw_on_open_failed(uint32_t job_id, const char *error, void *ctx) {
    (void)ctx;
    if (!lw_is_ours(job_id, "OPEN_FAILED"))
        return;
    lw_fail(error && *error ? error : "the interpreter refused the job");
}

// FED: the feed ran; its output is queued and the next read may go out.
static void lw_on_fed(uint32_t job_id, uint32_t sequence, laserwriter_feed_status_t status, uint32_t pages,
                      const uint8_t *reply, size_t reply_len, const uint8_t *errors, size_t errors_len, bool truncated,
                      void *ctx) {
    (void)ctx;
    (void)sequence;
    if (!lw_is_ours(job_id, "FED") || g_lw.state != JOB_FEEDING)
        return;
    if (status == LASERWRITER_FEED_FAILED) {
        lw_fail("feed failed");
        return;
    }
    if (truncated)
        LOG(1, "laserwriter: job %u: output truncated by the transport", (unsigned)job_id);
    if (status == LASERWRITER_FEED_ENDED && !g_lw.ended_early) {
        LOG(3, "laserwriter: job %u ended before its data did; later data is discarded", (unsigned)job_id);
        g_lw.ended_early = true;
    }
    g_lw.pages = pages;
    g_lw.state = JOB_READY;
    lw_take_output(reply, reply_len, errors, errors_len);
    lw_notify(LASERWRITER_EVENT_FED, NULL);
}

// FINISHED: the document is counted and handed to the sink; the job is over.
static void lw_on_finished(uint32_t job_id, const laserwriter_finish_result_t *res, void *ctx) {
    (void)ctx;
    if (!lw_is_ours(job_id, "FINISHED") || g_lw.state != JOB_FINISHING)
        return;
    lw_take_output(res->reply, res->reply_len, res->errors, res->errors_len);
    if (g_lw.output_dropped) {
        LOG(1, "laserwriter: job %u: %zu output bytes dropped (unread output over %u bytes)", (unsigned)job_id,
            g_lw.output_dropped, LASERWRITER_OUTPUT_MAX);
        g_lw.output_dropped = 0;
    }
    laserwriter_document_t doc;
    memset(&doc, 0, sizeof(doc));
    doc.job_id = job_id;
    doc.title = g_lw.title;
    doc.pdf = res->pdf;
    doc.pdf_len = res->pdf_len;
    doc.pages = res->pages;
    doc.error_name = res->error_name ? res->error_name : "";
    doc.offending = res->offending ? res->offending : "";
    doc.ok = res->outcome == LASERWRITER_OUTCOME_OK;
    doc.budget_exceeded = res->outcome == LASERWRITER_OUTCOME_BUDGET;
    g_lw.last_pages = doc.pages;
    g_lw.pages = doc.pages;
    if (res->outcome == LASERWRITER_OUTCOME_FAILED) {
        // The document could not be closed or the job is poisoned: nothing to keep
        LOG(1, "laserwriter: job %u finish failed: %s", (unsigned)job_id, doc.error_name);
        snprintf(g_lw.last_outcome, sizeof(g_lw.last_outcome), "failed: %s", doc.error_name);
    } else if (doc.budget_exceeded) {
        LOG(1, "laserwriter: job %u '%s' stopped by the execution budget after %u pages", (unsigned)job_id, g_lw.title,
            (unsigned)doc.pages);
        snprintf(g_lw.last_outcome, sizeof(g_lw.last_outcome), "budget");
    } else if (!doc.ok) {
        LOG(1, "laserwriter: job %u '%s' error: %s in %s (%u pages kept)", (unsigned)job_id, g_lw.title, doc.error_name,
            doc.offending, (unsigned)doc.pages);
        snprintf(g_lw.last_outcome, sizeof(g_lw.last_outcome), "error: %s in %s", doc.error_name, doc.offending);
    } else {
        LOG(2, "laserwriter: job %u '%s' complete: %u pages, %zu bytes", (unsigned)job_id, g_lw.title,
            (unsigned)doc.pages, doc.pdf_len);
        snprintf(g_lw.last_outcome, sizeof(g_lw.last_outcome), "ok");
    }
    // A job that shows no page and ends cleanly is a query or a placeholder:
    // a LaserWriter prints nothing for it.  An error keeps its document.
    // The ring transport has no bytes here (the worker posted the PDF to
    // the page); the count still moves.
    if (res->outcome != LASERWRITER_OUTCOME_FAILED && (doc.pages > 0 || !doc.ok)) {
        g_lw.documents++;
        if (doc.pdf)
            laserwriter_sink_document(&doc);
    }
    lw_release();
    lw_notify(LASERWRITER_EVENT_FINISHED, NULL);
}

// ============================================================================
// Operations (Public API)
// ============================================================================

bool laserwriter_job_available(void) {
    return true;
}

void laserwriter_job_set_listener(laserwriter_listener_t fn, void *ctx) {
    static const laserwriter_transport_callbacks_t callbacks = {
        .on_opened = lw_on_opened,
        .on_open_failed = lw_on_open_failed,
        .on_fed = lw_on_fed,
        .on_finished = lw_on_finished,
    };
    g_lw.listener = fn;
    g_lw.listener_ctx = ctx;
    laserwriter_transport_set_callbacks(&callbacks, NULL);
}

bool laserwriter_job_begin(uint32_t job_id) {
    if (g_lw.state != JOB_IDLE)
        laserwriter_job_abort();
    // The three identity values are the ones systemdict must agree with
    static const laserwriter_identity_t identity[] = {
        {"product",  "(" LASERWRITER_PRODUCT ")"},
        {"version",  "(" LASERWRITER_VERSION ")"},
        {"revision", LASERWRITER_REVISION       },
    };
    laserwriter_job_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.identity = identity;
    cfg.identity_len = sizeof(identity) / sizeof(identity[0]);
    cfg.prelude = laserwriter_prelude_ps;
    cfg.prelude_len = sizeof(laserwriter_prelude_ps);
    cfg.compress = true;
    cfg.embed_all_fonts = false; // the driver downloads what it needs; resident faces stay references
    cfg.step_budget = LASERWRITER_STEP_BUDGET;
    g_lw.job_id = job_id;
    g_lw.title[0] = '\0';
    g_lw.title_state = TITLE_SEARCHING;
    g_lw.title_window_len = 0;
    g_lw.pages = 0;
    g_lw.ended_early = false;
    if (!laserwriter_transport_open(job_id, &cfg)) {
        LOG(1, "laserwriter: job %u: the transport refused the open", (unsigned)job_id);
        g_lw.job_id = 0;
        return false;
    }
    g_lw.state = JOB_OPENING;
    scheduler_t *sched = atalk_scheduler();
    g_lw.open_deadline_ns = (sched ? scheduler_time_ns(sched) : 0.0) + (double)LASERWRITER_OPEN_TIMEOUT_NS;
    lw_poll_arm();
    LOG(2, "laserwriter: job %u opening", (unsigned)job_id);
    return true;
}

bool laserwriter_job_active(void) {
    return g_lw.state != JOB_IDLE;
}

bool laserwriter_job_ready(void) {
    return g_lw.state == JOB_READY;
}

bool laserwriter_job_feed_pending(void) {
    return g_lw.state == JOB_FEEDING || g_lw.state == JOB_FINISHING;
}

bool laserwriter_job_feed(uint32_t sequence, const uint8_t *data, size_t len) {
    if (g_lw.state != JOB_READY) {
        LOG(1, "laserwriter: job %u: feed while not ready (state %d)", (unsigned)g_lw.job_id, (int)g_lw.state);
        return false;
    }
    if (data && len)
        lw_scan_title(data, len);
    if (!laserwriter_transport_feed(g_lw.job_id, sequence, data, len)) {
        lw_fail("the transport refused the feed");
        return false;
    }
    g_lw.state = JOB_FEEDING;
    lw_poll_arm();
    return true;
}

size_t laserwriter_job_read_output(uint8_t *buf, size_t cap) {
    if (!buf || cap == 0 || g_lw.output_len == 0)
        return 0;
    size_t n = cap < g_lw.output_len ? cap : g_lw.output_len;
    memcpy(buf, g_lw.output, n);
    memmove(g_lw.output, g_lw.output + n, g_lw.output_len - n);
    g_lw.output_len -= n;
    return n;
}

bool laserwriter_job_finish(void) {
    if (g_lw.state != JOB_READY) {
        LOG(1, "laserwriter: job %u: finish while not ready (state %d)", (unsigned)g_lw.job_id, (int)g_lw.state);
        return false;
    }
    if (!laserwriter_transport_finish(g_lw.job_id)) {
        lw_fail("the transport refused the finish");
        return false;
    }
    g_lw.state = JOB_FINISHING;
    lw_poll_arm();
    return true;
}

void laserwriter_job_abort(void) {
    if (g_lw.state == JOB_IDLE)
        return;
    LOG(2, "laserwriter: job %u '%s' abandoned mid-job", (unsigned)g_lw.job_id, g_lw.title);
    laserwriter_transport_abandon(g_lw.job_id);
    lw_release();
    // Output of an abandoned job has no reader
    g_lw.output_len = 0;
    g_lw.output_dropped = 0;
}

void laserwriter_job_status(char *out, size_t cap) {
    if (!out || cap == 0)
        return;
    if (g_lw.state == JOB_IDLE) {
        snprintf(out, cap, "status: idle");
        return;
    }
    if (g_lw.state == JOB_OPENING) {
        snprintf(out, cap, "status: starting up");
        return;
    }
    uint32_t pages = g_lw.pages;
    const char *state = pages > 0 ? "printing" : "busy";
    if (g_lw.title[0])
        snprintf(out, cap, "status: %s; source: AppleTalk; job: %s", state, g_lw.title);
    else
        snprintf(out, cap, "status: %s; source: AppleTalk", state);
    if (pages > 0) {
        size_t n = strlen(out);
        if (n < cap)
            snprintf(out + n, cap - n, "; page: %u", (unsigned)pages);
    }
}

uint32_t laserwriter_job_pages(void) {
    return g_lw.state == JOB_IDLE ? 0 : g_lw.pages;
}

uint32_t laserwriter_job_documents(void) {
    return g_lw.documents;
}

uint32_t laserwriter_job_last_pages(void) {
    return g_lw.last_pages;
}

const char *laserwriter_job_last_outcome(void) {
    return g_lw.last_outcome;
}

#else // !GS_PLATEN

// ============================================================================
// Stubs for builds without the interpreter
// ============================================================================

bool laserwriter_job_available(void) {
    return false;
}

void laserwriter_job_set_listener(laserwriter_listener_t fn, void *ctx) {
    (void)fn;
    (void)ctx;
}

bool laserwriter_job_begin(uint32_t job_id) {
    (void)job_id;
    return false;
}

bool laserwriter_job_active(void) {
    return false;
}

bool laserwriter_job_ready(void) {
    return false;
}

bool laserwriter_job_feed_pending(void) {
    return false;
}

bool laserwriter_job_feed(uint32_t sequence, const uint8_t *data, size_t len) {
    (void)sequence;
    (void)data;
    (void)len;
    return false;
}

size_t laserwriter_job_read_output(uint8_t *buf, size_t cap) {
    (void)buf;
    (void)cap;
    return 0;
}

bool laserwriter_job_finish(void) {
    return false;
}

void laserwriter_job_abort(void) {}

void laserwriter_job_status(char *out, size_t cap) {
    if (out && cap)
        snprintf(out, cap, "status: idle");
}

uint32_t laserwriter_job_pages(void) {
    return 0;
}

uint32_t laserwriter_job_documents(void) {
    return 0;
}

uint32_t laserwriter_job_last_pages(void) {
    return 0;
}

const char *laserwriter_job_last_outcome(void) {
    return "";
}

#endif // GS_PLATEN
