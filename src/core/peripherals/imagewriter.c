// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// imagewriter.c
// An ImageWriter-class printer on the far end of one SCC channel's cable.
// It asserts the port's ready line, takes every asynchronous byte the guest
// transmits, and ends a job after IMAGEWRITER_IDLE_NS of guest time with no
// byte.  Finished jobs go to imagewriter_sink_job as raw ImageWriter command
// streams.  See imagewriter.h and docs/core/peripherals/imagewriter.md.

#include "imagewriter.h"

#include "log.h"
#include "machine_profile.h" // machine_object()
#include "object.h"
#include "scheduler.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("imagewriter");

// ============================================================================
// Type Definitions (Private)
// ============================================================================

// The printer: its port, the job being received, and what it has printed.
struct imagewriter {
    scc_t *scc;
    unsigned int ch;
    struct scheduler *scheduler;
    uint8_t *job; // bytes of the job in progress
    size_t job_len;
    size_t job_cap;
    bool job_truncated; // the job reached IMAGEWRITER_JOB_MAX
    uint32_t jobs; // jobs handed to the sink
    uint64_t bytes; // bytes received in total
    uint8_t *last; // a copy of the last finished job, for scripts
    size_t last_len;
    struct object *object; // `machine.printer`
};

// ============================================================================
// Forward Declarations
// ============================================================================

static void iw_idle_cb(void *source, uint64_t data);
static const class_desc_t iw_class;

// ============================================================================
// Platform sink default
// ============================================================================

// Fallback when no platform sink is linked: the job is lost, loudly.
__attribute__((weak)) void imagewriter_sink_job(const imagewriter_job_t *job) {
    LOG(1, "imagewriter: job %u (%zu bytes) dropped: no job sink on this platform", (unsigned)job->job_id, job->len);
}

// ============================================================================
// Static Helpers
// ============================================================================

// Hands the job in progress to the sink and starts the next one empty.
static void iw_finish_job(imagewriter_t *iw) {
    remove_event(iw->scheduler, &iw_idle_cb, iw);
    if (!iw->job_len)
        return;
    imagewriter_job_t job = {.job_id = ++iw->jobs, .data = iw->job, .len = iw->job_len, .truncated = iw->job_truncated};
    LOG(2, "imagewriter: job %u complete (%zu bytes%s)", (unsigned)job.job_id, job.len,
        job.truncated ? ", truncated" : "");
    imagewriter_sink_job(&job);
    // Keep a copy for `printer.last_job()`
    uint8_t *copy = (uint8_t *)realloc(iw->last, iw->job_len);
    if (copy) {
        memcpy(copy, iw->job, iw->job_len);
        iw->last = copy;
        iw->last_len = iw->job_len;
    }
    iw->job_len = 0;
    iw->job_truncated = false;
}

// Appends one byte to the job in progress, growing the buffer as needed.
static void iw_append(imagewriter_t *iw, uint8_t byte) {
    if (iw->job_len == iw->job_cap) {
        size_t cap = iw->job_cap ? iw->job_cap * 2 : 4096;
        if (cap > IMAGEWRITER_JOB_MAX)
            cap = IMAGEWRITER_JOB_MAX;
        uint8_t *grown = (uint8_t *)realloc(iw->job, cap);
        if (!grown) {
            LOG(1, "imagewriter: out of memory at %zu bytes; job cut short", iw->job_len);
            iw->job_truncated = true;
            iw_finish_job(iw);
            return;
        }
        iw->job = grown;
        iw->job_cap = cap;
    }
    iw->job[iw->job_len++] = byte;
    // A runaway stream is handed over at the cap and continues as a new job
    if (iw->job_len == IMAGEWRITER_JOB_MAX) {
        iw->job_truncated = true;
        iw_finish_job(iw);
    }
}

// The cable: one byte the guest transmitted.  Re-arms the end-of-job timer.
static void iw_on_byte(void *context, uint8_t byte) {
    imagewriter_t *iw = (imagewriter_t *)context;
    if (!iw->job_len)
        LOG(2, "imagewriter: job %u started", (unsigned)(iw->jobs + 1));
    iw->bytes++;
    iw_append(iw, byte);
    remove_event(iw->scheduler, &iw_idle_cb, iw);
    scheduler_new_cpu_event(iw->scheduler, &iw_idle_cb, iw, 0, 0, IMAGEWRITER_IDLE_NS);
}

// The line has been quiet for IMAGEWRITER_IDLE_NS: the job is over.
static void iw_idle_cb(void *source, uint64_t data) {
    (void)data;
    iw_finish_job((imagewriter_t *)source);
}

// ============================================================================
// Lifecycle (Constructor / Destructor)
// ============================================================================

imagewriter_t *imagewriter_init(scc_t *scc, unsigned int ch, scc_pin_t ready_pin, bool ready_asserted,
                                struct scheduler *scheduler) {
    if (!scc || ch > 1 || !scheduler)
        return NULL;
    imagewriter_t *iw = (imagewriter_t *)calloc(1, sizeof(*iw));
    if (!iw)
        return NULL;
    iw->scc = scc;
    iw->ch = ch;
    iw->scheduler = scheduler;
    // Registered before any checkpoint restore replays the event queue
    scheduler_new_event_type(scheduler, "imagewriter", iw, "idle", &iw_idle_cb);
    // Power on: the ready line goes to its "ready" level and stays there
    scc_set_input_pin(scc, ch, ready_pin, ready_asserted);
    scc_set_tx_byte_sink(scc, ch, iw_on_byte, iw);
    iw->object = object_new(&iw_class, iw, "printer");
    if (iw->object) {
        object_set_label(iw->object, "ImageWriter");
        object_set_order(iw->object, 120);
        object_attach(machine_object(), iw->object);
    }
    LOG(2, "imagewriter: on SCC channel %c", ch ? 'B' : 'A');
    return iw;
}

void imagewriter_delete(imagewriter_t *iw) {
    if (!iw)
        return;
    iw_finish_job(iw); // what was received is printed, as when a printer's buffer drains
    scc_set_tx_byte_sink(iw->scc, iw->ch, NULL, NULL);
    if (iw->object) {
        object_detach(iw->object);
        object_delete(iw->object);
    }
    free(iw->job);
    free(iw->last);
    free(iw);
}

// ============================================================================
// Operations (Public API)
// ============================================================================

void imagewriter_flush(imagewriter_t *iw) {
    if (iw)
        iw_finish_job(iw);
}

uint32_t imagewriter_jobs(const imagewriter_t *iw) {
    return iw ? iw->jobs : 0;
}

uint64_t imagewriter_bytes(const imagewriter_t *iw) {
    return iw ? iw->bytes : 0;
}

size_t imagewriter_pending(const imagewriter_t *iw) {
    return iw ? iw->job_len : 0;
}

// ============================================================================
// Object model: `machine.printer`
// ============================================================================

static imagewriter_t *iw_from(struct object *self) {
    return (imagewriter_t *)object_data(self);
}

static value_t iw_attr_jobs(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, imagewriter_jobs(iw_from(self)));
}

static value_t iw_attr_bytes(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(8, imagewriter_bytes(iw_from(self)));
}

static value_t iw_attr_pending(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(8, imagewriter_pending(iw_from(self)));
}

static value_t iw_attr_last_len(struct object *self, const member_t *m) {
    (void)m;
    imagewriter_t *iw = iw_from(self);
    return val_uint(8, iw ? iw->last_len : 0);
}

// `flush()`: end the job in progress now.
static value_t iw_method_flush(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    imagewriter_flush(iw_from(self));
    return val_none();
}

// `last_job()`: the last finished job as text, printable ASCII kept and
// every other byte escaped `\xNN` (the scc.<ch>.sent() convention), so a
// script can assert on what the driver sent.
static value_t iw_method_last_job(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    imagewriter_t *iw = iw_from(self);
    if (!iw || !iw->last_len)
        return val_str("");
    char *text = (char *)malloc(iw->last_len * 4 + 1);
    if (!text)
        return val_err("last_job: out of memory");
    size_t o = 0;
    for (size_t i = 0; i < iw->last_len; i++) {
        uint8_t b = iw->last[i];
        if (b == '\\') {
            text[o++] = '\\';
            text[o++] = '\\';
        } else if (b == '\t' || b == '\n' || b == '\r' || (b >= 0x20 && b < 0x7F)) {
            text[o++] = (char)b;
        } else {
            o += (size_t)snprintf(text + o, 5, "\\x%02X", b);
        }
    }
    text[o] = '\0';
    value_t v = val_str(text);
    free(text);
    return v;
}

static const member_t iw_members[] = {
    {.kind = M_ATTR,
     .name = "jobs",
     .flags = VAL_RO,
     .doc = "Jobs printed so far (each handed to the platform's job sink)",
     .attr = {.type = V_UINT, .get = iw_attr_jobs}},
    {.kind = M_ATTR,
     .name = "bytes",
     .flags = VAL_RO,
     .doc = "Bytes received from the guest since power-on",
     .attr = {.type = V_UINT, .get = iw_attr_bytes}},
    {.kind = M_ATTR,
     .name = "pending",
     .flags = VAL_RO,
     .doc = "Bytes of the job in progress; it ends after 5 s of guest time with no data",
     .attr = {.type = V_UINT, .get = iw_attr_pending}},
    {.kind = M_ATTR,
     .name = "last_len",
     .flags = VAL_RO,
     .doc = "Bytes in the last finished job",
     .attr = {.type = V_UINT, .get = iw_attr_last_len}},
    {.kind = M_METHOD,
     .name = "flush",
     .doc = "End the job in progress now",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = iw_method_flush}},
    {.kind = M_METHOD,
     .name = "last_job",
     .doc = "The last finished job as text (non-printing bytes escaped \\xNN)",
     .method = {.args = NULL, .nargs = 0, .result = V_STRING, .fn = iw_method_last_job}},
};

static const class_desc_t iw_class = {
    .name = "imagewriter",
    .members = iw_members,
    .n_members = sizeof(iw_members) / sizeof(iw_members[0]),
};
