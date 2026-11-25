// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// job.c -- see job.h.  Everything here runs on the emulator thread: jobs are
// queued by the drain (serve_script), stepped by the drain
// (job_layer_service), and cancelled by the drain (REQ_CANCEL) or by a
// leaf a job's own task called.

#include "job.h"

#include "json_escape.h"
#include "platform_hooks.h"
#include "io/io_worker.h"
#include "mailbox/mailbox.h"
#include "object/json_value.h"
#include "object/value_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct job {
    struct job *next; // the queue, or the finished list
    uint32_t client;
    uint32_t req_id;
    char *src;
    job_task_t *task; // made at the first step
    bool started;
    bool cancel;
    bool waiting; // suspended until its mode ends and its deferred answer comes
    int rc;
    uint32_t mode_id; // the mode a call of this job started (0: none)
    // Output the job printed and the drain has not yet delivered: a growable
    // buffer, cut at JOB_OUTPUT_MAX.
    char *out;
    size_t out_len, out_cap;
    bool out_cut;
    // Bytes of output already written to the ring: out[0] is byte
    // out_base of everything the job has printed.
    uint64_t out_base;
    // Pending annotations (job_annotate), in order: each is written once
    // the text before its position has been.
    struct job_annot *annot_head, *annot_tail;
};

// One annotation record waiting for its position in the output stream.
struct job_annot {
    uint64_t at; // absolute output offset it follows
    char *json;
    struct job_annot *next;
};
#define JOB_OUTPUT_MAX (1u << 20)
// Room an output record needs besides its escaped text: the record header
// and the {"event":"output","id":…,"client":…,"text":"…"} envelope.
#define JOB_RECORD_OVERHEAD 128u

// Jobs waiting (FIFO), the active one, and those finished whose result is
// not yet written, in the order they finished.
static job_t *g_queue_head, *g_queue_tail;
static job_t *g_active;
static job_t *g_done_head, *g_done_tail;

// The step in progress: the active job's task is running.
static bool g_stepping;
// Whether the tree call in progress may suspend the task (and so defer).
static bool g_can_suspend;

// The deferred answer of the active job's call (one at a time: a task
// suspends on it).
static uint32_t g_defer_token; // 0: none
static uint32_t g_defer_io; // the I/O job answering it (0: none)
static uint32_t g_defer_req; // the request id of the job it belongs to
static value_t g_call_answer;
static bool g_call_answered;

// A deferred answer that failed: noted when the completion lands, taken by
// the engine when the task resumes.
static bool g_call_failed;
static char g_call_error[256];

static void (*g_frame_runner)(void);
static bool g_in_frame_wait;

static volatile uint32_t *g_wake_word; // the mailbox's REQ_HEAD (the emulator parks on it)

void job_layer_set_wake_word(volatile uint32_t *word) {
    g_wake_word = word;
}

static void job_free(job_t *j) {
    while (j->annot_head) {
        struct job_annot *a = j->annot_head;
        j->annot_head = a->next;
        free(a->json);
        free(a);
    }
    if (j->task)
        job_glue_task_free(j->task);
    free(j->src);
    free(j->out);
    free(j);
}

static void done_push(job_t *j) {
    j->next = NULL;
    if (g_done_tail)
        g_done_tail->next = j;
    else
        g_done_head = j;
    g_done_tail = j;
}

// --- output ------------------------------------------------------------------

static void job_append(job_t *j, const char *text, size_t len) {
    if (j->out_cut)
        return;
    if (j->out_len + len > JOB_OUTPUT_MAX) {
        len = JOB_OUTPUT_MAX - j->out_len;
        j->out_cut = true;
    }
    if (j->out_len + len + 4 > j->out_cap) {
        size_t cap = j->out_cap ? j->out_cap : 4096;
        while (cap < j->out_len + len + 4)
            cap *= 2;
        char *n = (char *)realloc(j->out, cap);
        if (!n)
            return;
        j->out = n;
        j->out_cap = cap;
    }
    memcpy(j->out + j->out_len, text, len);
    j->out_len += len;
    if (j->out_cut) {
        memcpy(j->out + j->out_len, "...", 3);
        j->out_len += 3;
    }
}

bool job_output_append(const char *text, size_t len) {
    job_t *j = job_current();
    if (!j)
        return false;
    job_append(j, text, len);
    return true;
}

// Writes the job's buffered output as EVT_LOG output records, and its
// annotations at their positions, as far as the ring has room.  Each text
// record is sized by its ESCAPED length so the record fits the ring's record
// bound (mailbox_record_max) -- a 16 KiB chunk of control bytes escapes to
// ~96 KiB, which could never fit the headless ring.  A text record never
// crosses an annotation's position.  Returns whether everything was written.
static bool job_flush_output(struct mailbox *m, job_t *j) {
    size_t budget = mailbox_record_max();
    budget = budget > JOB_RECORD_OVERHEAD + 64 ? budget - JOB_RECORD_OVERHEAD : 64;
    for (;;) {
        // An annotation whose position has been reached goes next.
        struct job_annot *a = j->annot_head;
        if (a && a->at <= j->out_base) {
            if (!mailbox_emit_output(m, a->json))
                return false;
            j->annot_head = a->next;
            if (!j->annot_head)
                j->annot_tail = NULL;
            free(a->json);
            free(a);
            continue;
        }
        if (!j->out_len)
            return true;
        // Text up to the next annotation's position, at most `budget`
        // escaped bytes.
        size_t limit = j->out_len;
        if (a && a->at - j->out_base < limit)
            limit = (size_t)(a->at - j->out_base);
        size_t n = 0, esc = 0;
        while (n < limit && esc + json_escaped_len((unsigned char)j->out[n]) <= budget)
            esc += json_escaped_len((unsigned char)j->out[n++]);
        // Do not split a UTF-8 sequence: back up to a boundary unless this
        // chunk ends where it must (the tail, or an annotation).
        if (n < limit)
            while (n > 1 && ((unsigned char)j->out[n] & 0xC0u) == 0x80u)
                n--;
        if (n == 0)
            n = 1;
        vbuf_t json = {0};
        vbuf_appendf(&json, "{\"event\":\"output\",\"id\":%u,\"client\":%u,\"text\":\"", (unsigned)j->req_id,
                     (unsigned)j->client);
        size_t head = json.len, body = 0;
        for (size_t i = 0; i < n; i++)
            body += json_escaped_len((unsigned char)j->out[i]);
        vbuf_append_json_escaped(&json, j->out, n);
        vbuf_append(&json, "\"}", 2);
        // A buffer an allocation left short is not a record
        if (!head || json.len != head + body + 2) {
            vbuf_free(&json);
            return true; // drop rather than wedge
        }
        bool ok = mailbox_emit_output(m, json.p);
        vbuf_free(&json);
        if (!ok)
            return false;
        memmove(j->out, j->out + n, j->out_len - n);
        j->out_len -= n;
        j->out_base += n;
    }
}

// The entries of map `m` as JSON object members, without the braces (tagged
// JSON, the bridge's form); "" for none.  Malloc'd, NULL on failure.
static char *annotation_fields(const value_t *m) {
    if (!m || m->kind != VK_MAP || m->map.len == 0)
        return strdup("");
    vbuf_t b = {0};
    value_format(m, VFMT_JSON_TAGGED, &b);
    if (!b.p || b.len < 2)
        return b.p;
    // Drop the enclosing `{` `}`.
    memmove(b.p, b.p + 1, b.len - 2);
    b.p[b.len - 2] = '\0';
    return b.p;
}

bool job_annotate(const char *kind, const value_t *fields, const value_t *reduced, bool *used_reduced) {
    if (used_reduced)
        *used_reduced = false;
    job_t *j = job_current();
    // Nothing to attach it to, or past the 1 MiB cut: dropped.
    if (!j || j->out_cut)
        return false;
    vbuf_t kjson = {0};
    value_t kv = val_str(kind ? kind : "");
    value_format(&kv, VFMT_JSON, &kjson);
    value_free(&kv);
    char *bodies[2] = {annotation_fields(fields), reduced ? annotation_fields(reduced) : NULL};
    size_t max = mailbox_record_max();
    max = max > 16 ? max - 16 : max; // the record header
    struct job_annot *a = (struct job_annot *)calloc(1, sizeof(*a));
    bool placed = false, shortened = false;
    if (a && kjson.p && bodies[0]) {
        for (int pass = 0; pass < 2 && !a->json; pass++) {
            const char *body = bodies[pass];
            if (!body)
                break;
            size_t need = 64 + kjson.len + strlen(body);
            char *buf = (char *)malloc(need);
            if (!buf)
                break;
            int n = snprintf(buf, need, "{\"event\":%s,\"id\":%u,\"client\":%u%s%s}", kjson.p, (unsigned)j->req_id,
                             (unsigned)j->client, *body ? "," : "", body);
            if (n > 0 && (size_t)n <= max) {
                a->json = buf;
                shortened = pass == 1;
            } else {
                free(buf);
            }
        }
        if (a->json) {
            a->at = j->out_base + j->out_len;
            if (j->annot_tail)
                j->annot_tail->next = a;
            else
                j->annot_head = a;
            j->annot_tail = a;
            placed = true;
        }
    }
    if (!placed)
        free(a);
    vbuf_free(&kjson);
    free(bodies[0]);
    free(bodies[1]);
    if (!placed)
        return false;
    if (used_reduced)
        *used_reduced = shortened;
    return true;
}

// The output every job has pending, to the ring.
static void jobs_flush_output(struct mailbox *m) {
    if (g_active)
        job_flush_output(m, g_active);
    for (job_t *j = g_done_head; j; j = j->next)
        job_flush_output(m, j);
}

// --- the running job ------------------------------------------------------

job_t *job_current(void) {
    return g_stepping ? g_active : NULL;
}

uint32_t job_current_client(void) {
    return g_stepping && g_active ? g_active->client : 0;
}

bool job_current_cancelled(void) {
    return g_stepping && g_active && g_active->cancel;
}

bool job_serving_call(void) {
    return g_stepping;
}

void job_set_frame_runner(void (*run_frame)(void)) {
    g_frame_runner = run_frame;
}

// Frames until `client`'s mode ends -- the wait of a call that cannot
// suspend.  The drain is not re-entered (a request may be mid-serve); the
// I/O worker's completions are, since a leaf may wait on one.
static void wait_with_frames(uint32_t client) {
    if (!g_frame_runner || g_in_frame_wait || !client)
        return;
    g_in_frame_wait = true;
    while (job_glue_mode_waits(client)) {
        g_frame_runner();
        io_worker_service();
        if (job_current_cancelled())
            break;
    }
    g_in_frame_wait = false;
}

job_call_mark_t job_task_call_begin(bool can_suspend) {
    job_call_mark_t mark = {
        .mode = job_glue_mode_id(),
        .prev_can_suspend = g_can_suspend,
        .in_job = g_stepping && g_active,
    };
    g_can_suspend = mark.in_job && can_suspend;
    return mark;
}

bool job_task_call_end(job_call_mark_t mark) {
    bool could_suspend = g_can_suspend;
    g_can_suspend = mark.prev_can_suspend;
    uint32_t after = job_glue_mode_id();
    bool new_mode = after != mark.mode;
    if (mark.in_job && g_active) {
        if (new_mode)
            g_active->mode_id = after; // what a cancel stops
        if (could_suspend) {
            bool wait = (new_mode && job_glue_mode_waits(g_active->client)) || g_defer_token != 0;
            g_active->waiting = wait;
            return wait;
        }
        if (new_mode)
            wait_with_frames(g_active->client);
        return false;
    }
    if (new_mode)
        wait_with_frames(mailbox_serving_client());
    return false;
}

// --- a call's deferred answer ---------------------------------------------------

static uint32_t g_defer_seq;

uint32_t job_call_defer(void) {
    if (!g_stepping || !g_active || !g_can_suspend || g_defer_token)
        return 0;
    g_defer_seq = (g_defer_seq + 1) & 0x7fffffffu;
    if (!g_defer_seq)
        g_defer_seq = 1;
    g_defer_token = g_defer_seq | 0x80000000u;
    g_defer_io = 0;
    g_defer_req = g_active->req_id;
    return g_defer_token;
}

void job_call_bind_io(uint32_t token, uint32_t io_job) {
    if (g_defer_token == token)
        g_defer_io = io_job;
}

uint32_t job_call_request_id(uint32_t token) {
    return g_defer_token == token ? g_defer_req : 0;
}

void job_call_complete(uint32_t token, bool ok, const char *json) {
    if (g_defer_token != token)
        return;
    g_defer_token = 0;
    g_defer_io = 0;
    if (!ok) {
        // The error text out of {"error":"..."}, decoded, whatever else the
        // object holds; anything that is not such a document verbatim.
        char error[256];
        value_t doc;
        bool parsed = json && json_value_parse(json, &doc, NULL, 0);
        const char *msg = parsed ? val_as_str(value_map_get(&doc, "error")) : NULL;
        snprintf(error, sizeof error, "%s", msg ? msg : (json ? json : "failed"));
        if (parsed)
            value_free(&doc);
        g_call_failed = true;
        snprintf(g_call_error, sizeof g_call_error, "%s", error);
    }
}

bool job_call_take_failure(char *err, size_t cap) {
    bool failed = g_call_failed;
    if (failed && err && cap)
        snprintf(err, cap, "%s", g_call_error);
    g_call_failed = false;
    return failed;
}

bool job_call_complete_value(uint32_t token, const value_t *v) {
    if (!(token & 0x80000000u))
        return false; // a request's deferral: its answer is JSON
    if (g_defer_token == token) {
        g_defer_token = 0;
        g_defer_io = 0;
        if (g_call_answered)
            value_free(&g_call_answer);
        g_call_answer = value_dup(v);
        g_call_answered = true;
    }
    return true;
}

bool job_call_take_answer(value_t *out) {
    bool got = g_call_answered;
    value_t v = g_call_answer;
    g_call_answered = false;
    memset(&g_call_answer, 0, sizeof g_call_answer);
    if (got && out)
        *out = v;
    else if (got)
        value_free(&v);
    return got;
}

// --- submission and cancel ---------------------------------------------------

bool job_submit_script(uint32_t client, uint32_t req_id, const char *src, size_t len) {
    job_t *j = (job_t *)calloc(1, sizeof(*j));
    if (!j)
        return false;
    j->src = (char *)malloc(len + 1);
    if (!j->src) {
        free(j);
        return false;
    }
    memcpy(j->src, src, len);
    j->src[len] = '\0';
    j->client = client;
    j->req_id = req_id;
    if (g_queue_tail)
        g_queue_tail->next = j;
    else
        g_queue_head = j;
    g_queue_tail = j;
    if (g_wake_word)
        platform_wake(g_wake_word);
    return true;
}

static int cancel_matching(uint32_t client, uint32_t req_id, bool any_req) {
    int n = 0;
    // Queued jobs never start: they finish at once, cancelled.
    job_t **pp = &g_queue_head;
    g_queue_tail = NULL;
    while (*pp) {
        job_t *j = *pp;
        if (j->client == client && (any_req || j->req_id == req_id)) {
            *pp = j->next;
            j->cancel = true;
            j->rc = -1;
            done_push(j);
            n++;
            continue;
        }
        g_queue_tail = j;
        pp = &j->next;
    }
    job_t *a = g_active;
    if (a && a->client == client && (any_req || a->req_id == req_id)) {
        a->cancel = true;
        n++;
        // The run the job started ends with it (the client's other runs
        // stay), and so does a deferred I/O call: the task then resumes and
        // unwinds.
        if (a->mode_id)
            job_glue_stop_mode(client, a->mode_id);
        if (g_defer_token && g_defer_io)
            io_worker_cancel(g_defer_io);
    }
    return n;
}

int job_cancel(uint32_t client, uint32_t req_id) {
    return cancel_matching(client, req_id, false);
}

int job_cancel_client(uint32_t client) {
    return cancel_matching(client, 0, true);
}

// --- the drain's side -----------------------------------------------------------

// Whether the active job can run now: not waiting, or its wait is over.
static bool active_runnable(void) {
    job_t *j = g_active;
    if (!j)
        return false;
    if (!j->waiting)
        return true;
    return g_defer_token == 0 && !job_glue_mode_waits(j->client);
}

bool job_layer_has_work(void) {
    if (g_done_head || (!g_active && g_queue_head))
        return true;
    job_t *a = g_active;
    return a && (a->out_len != 0 || a->annot_head != NULL || active_runnable());
}

// One step of the active job's task.
static void step_active(struct mailbox *m) {
    job_t *j = g_active;
    uint32_t prev_client = m->client;
    m->client = j->client;
    g_stepping = true;
    bool finished;
    int rc = -1;
    if (!j->started) {
        j->started = true;
        job_call_take_failure(NULL, 0); // a failure the last job never took is not this one's
        job_call_take_answer(NULL); // nor an answer
        j->task = j->cancel ? NULL : job_glue_task_new(j->src);
        finished = j->task == NULL;
        if (!finished)
            finished = job_glue_task_step(j->task, &rc);
    } else {
        j->waiting = false;
        finished = job_glue_task_step(j->task, &rc);
    }
    g_stepping = false;
    g_can_suspend = false;
    m->client = prev_client;
    if (finished) {
        j->rc = j->cancel ? -1 : rc;
        g_active = NULL;
        // A deferral the task left behind (it unwound while waiting) is
        // answered into nothing.
        g_defer_token = 0;
        g_defer_io = 0;
        done_push(j);
    }
}

int job_layer_service(struct mailbox *m) {
    int written = 0;
    // 0. Output the jobs printed since the last drain, before anything
    // that might answer them.
    jobs_flush_output(m);
    // 1. The next job, when none is active.
    if (!g_active && g_queue_head) {
        g_active = g_queue_head;
        g_queue_head = g_active->next;
        if (!g_queue_head)
            g_queue_tail = NULL;
        g_active->next = NULL;
    }
    // 2. One step of the active job, when it can run.
    if (active_runnable()) {
        step_active(m);
        jobs_flush_output(m);
    }
    // 3. Finished jobs: their result is the shell's prompt (what shell.run
    // returned), or an error.
    while (g_done_head) {
        job_t *j = g_done_head;
        bool ok;
        if (j->rc == 0) {
            int rc = m->eval("shell.prompt", NULL, m->out, GS_MBX_RESULT_OUT);
            ok = rc == 0;
        } else {
            snprintf(m->out, GS_MBX_RESULT_MAX, "{\"error\":\"%s\"}", j->cancel ? "cancelled" : "command failed");
            ok = false;
        }
        // Its output first, then the result (a client sees them in order).
        if (!job_flush_output(m, j))
            break;
        if (!mailbox_write_result(m, j->req_id, ok, m->out, (uint32_t)strlen(m->out), NULL, 0))
            break; // no room: keep it for the next drain
        written++;
        g_done_head = j->next;
        if (!g_done_head)
            g_done_tail = NULL;
        job_free(j);
    }
    return written;
}
