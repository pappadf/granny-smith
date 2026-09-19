// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// laserwriter_job.c
// One platen job per EOF-delimited PAP job: feed, drain, finish, and the
// status text and document hand-off.  See laserwriter_job.h.

#include "laserwriter_job.h"

#include "log.h"

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
#include "platen.h"

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

// Longest reply or error piece pulled from the library per call.
#define LASERWRITER_DRAIN_CHUNK 4096

// ============================================================================
// Type Definitions (Private)
// ============================================================================

// Where the %%Title: scanner is.
typedef enum { TITLE_SEARCHING = 0, TITLE_COLLECTING, TITLE_DONE } title_scan_state_t;

// The single job (a LaserWriter serves one connection at a time).
typedef struct {
    platen_job *job;
    uint32_t job_id;
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
} laserwriter_state_t;

static laserwriter_state_t g_lw;

// ============================================================================
// Static Helpers
// ============================================================================

// Appends `len` bytes to the unread-output buffer, dropping past the cap.
static void lw_output_append(const uint8_t *bytes, size_t len) {
    if (len == 0)
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

// Pulls everything the library has written on both channels into the
// unread-output buffer: the reply channel first, then the error reports.
static void lw_drain(void) {
    uint8_t chunk[LASERWRITER_DRAIN_CHUNK];
    size_t n;
    while ((n = platen_job_read_replies(g_lw.job, chunk, sizeof(chunk))) > 0) {
        LOG(3, "laserwriter: job %u reply %zu bytes", (unsigned)g_lw.job_id, n);
        lw_output_append(chunk, n);
    }
    while ((n = platen_job_read_errors(g_lw.job, chunk, sizeof(chunk))) > 0) {
        LOG(2, "laserwriter: job %u error report: %.*s", (unsigned)g_lw.job_id, (int)(n > 200 ? 200 : n), chunk);
        lw_output_append(chunk, n);
    }
}

// Scans fed bytes for the driver's "%%Title: <name>" line and keeps the
// name; the pattern and the line may each span feeds.
static void lw_scan_title(const uint8_t *data, size_t len) {
    static const char pattern[] = "%%Title:";
    const size_t pattern_len = sizeof(pattern) - 1;
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

// Releases the library job and the per-job fields; output stays readable.
static void lw_release(void) {
    if (g_lw.job) {
        platen_job_free(g_lw.job);
        g_lw.job = NULL;
    }
    g_lw.title[0] = '\0';
    g_lw.title_state = TITLE_SEARCHING;
    g_lw.title_window_len = 0;
}

// ============================================================================
// Operations (Public API)
// ============================================================================

bool laserwriter_job_available(void) {
    return true;
}

bool laserwriter_job_begin(uint32_t job_id) {
    if (g_lw.job)
        lw_release();
    // The three identity values are the ones systemdict must agree with
    static const platen_entry identity[] = {
        {"product",  "(" LASERWRITER_PRODUCT ")"},
        {"version",  "(" LASERWRITER_VERSION ")"},
        {"revision", LASERWRITER_REVISION       },
    };
    platen_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.abi_version = PLATEN_ABI_VERSION;
    cfg.identity = identity;
    cfg.identity_len = sizeof(identity) / sizeof(identity[0]);
    cfg.prelude = laserwriter_prelude_ps;
    cfg.prelude_len = sizeof(laserwriter_prelude_ps);
    cfg.compress = 1;
    cfg.embed_all_fonts = 0; // the driver downloads what it needs; resident faces stay references
    cfg.step_budget = LASERWRITER_STEP_BUDGET;
    g_lw.job = platen_job_new(&cfg);
    if (!g_lw.job) {
        LOG(1, "laserwriter: job %u: interpreter refused the configuration: %s", (unsigned)job_id, platen_last_error());
        return false;
    }
    g_lw.job_id = job_id;
    g_lw.title[0] = '\0';
    g_lw.title_state = TITLE_SEARCHING;
    g_lw.title_window_len = 0;
    LOG(2, "laserwriter: job %u started", (unsigned)job_id);
    return true;
}

bool laserwriter_job_active(void) {
    return g_lw.job != NULL;
}

void laserwriter_job_feed(const uint8_t *data, size_t len) {
    if (!g_lw.job || !data || len == 0)
        return;
    lw_scan_title(data, len);
    int rc = platen_job_feed(g_lw.job, data, len);
    if (rc < 0)
        LOG(1, "laserwriter: job %u feed failed (%d): %s", (unsigned)g_lw.job_id, rc, platen_last_error());
    else if (rc == PLATEN_DONE)
        LOG(3, "laserwriter: job %u ended before its data did; %zu bytes discarded", (unsigned)g_lw.job_id, len);
    lw_drain();
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

void laserwriter_job_finish(void) {
    if (!g_lw.job)
        return;
    int outcome = platen_job_finish(g_lw.job);
    lw_drain();
    if (g_lw.output_dropped) {
        LOG(1, "laserwriter: job %u: %zu output bytes dropped (unread output over %u bytes)", (unsigned)g_lw.job_id,
            g_lw.output_dropped, LASERWRITER_OUTPUT_MAX);
        g_lw.output_dropped = 0;
    }
    laserwriter_document_t doc;
    memset(&doc, 0, sizeof(doc));
    doc.job_id = g_lw.job_id;
    doc.title = g_lw.title;
    doc.pdf = platen_job_pdf(g_lw.job, &doc.pdf_len);
    doc.pages = platen_job_pages(g_lw.job);
    doc.error_name = platen_job_error_name(g_lw.job);
    doc.offending = platen_job_offending(g_lw.job);
    doc.ok = outcome == PLATEN_OUTCOME_OK;
    doc.budget_exceeded = outcome == PLATEN_OUTCOME_BUDGET;
    g_lw.last_pages = doc.pages;
    if (outcome < 0) {
        // The document could not be closed or the job is poisoned: nothing to keep
        LOG(1, "laserwriter: job %u finish failed (%d): %s", (unsigned)g_lw.job_id, outcome, platen_last_error());
        snprintf(g_lw.last_outcome, sizeof(g_lw.last_outcome), "failed: %s", platen_last_error());
    } else if (doc.budget_exceeded) {
        LOG(1, "laserwriter: job %u '%s' stopped by the execution budget after %u pages", (unsigned)g_lw.job_id,
            g_lw.title, (unsigned)doc.pages);
        snprintf(g_lw.last_outcome, sizeof(g_lw.last_outcome), "budget");
    } else if (!doc.ok) {
        LOG(1, "laserwriter: job %u '%s' error: %s in %s (%u pages kept)", (unsigned)g_lw.job_id, g_lw.title,
            doc.error_name, doc.offending, (unsigned)doc.pages);
        snprintf(g_lw.last_outcome, sizeof(g_lw.last_outcome), "error: %s in %s", doc.error_name, doc.offending);
    } else {
        LOG(2, "laserwriter: job %u '%s' complete: %u pages, %zu bytes", (unsigned)g_lw.job_id, g_lw.title,
            (unsigned)doc.pages, doc.pdf_len);
        snprintf(g_lw.last_outcome, sizeof(g_lw.last_outcome), "ok");
    }
    // A job that shows no page and ends cleanly is a query or a placeholder:
    // a LaserWriter prints nothing for it.  An error keeps its document.
    if (outcome >= 0 && doc.pdf && (doc.pages > 0 || !doc.ok)) {
        g_lw.documents++;
        laserwriter_sink_document(&doc);
    }
    lw_release();
}

void laserwriter_job_abort(void) {
    if (!g_lw.job)
        return;
    LOG(2, "laserwriter: job %u '%s' abandoned mid-job", (unsigned)g_lw.job_id, g_lw.title);
    lw_release();
    // Output of an abandoned job has no reader
    g_lw.output_len = 0;
    g_lw.output_dropped = 0;
}

void laserwriter_job_status(char *out, size_t cap) {
    if (!out || cap == 0)
        return;
    if (!g_lw.job) {
        snprintf(out, cap, "status: idle");
        return;
    }
    uint32_t pages = platen_job_pages(g_lw.job);
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
    return g_lw.job ? platen_job_pages(g_lw.job) : 0;
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

bool laserwriter_job_begin(uint32_t job_id) {
    (void)job_id;
    return false;
}

bool laserwriter_job_active(void) {
    return false;
}

void laserwriter_job_feed(const uint8_t *data, size_t len) {
    (void)data;
    (void)len;
}

size_t laserwriter_job_read_output(uint8_t *buf, size_t cap) {
    (void)buf;
    (void)cap;
    return 0;
}

void laserwriter_job_finish(void) {}

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
