// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// job.c -- see job.h.

#include "job.h"

#include "io/io_worker.h"
#include "mailbox/mailbox.h"
#include "object/value_format.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gs_job {
    uint32_t client;
    uint32_t req_id;
    char *src;
    bool cancel;
    int rc;
    uint32_t mode_id; // the mode a call of this job started (0: none)
    // Output the job printed and the emulator thread has not yet delivered
    // (g_mu): a growable buffer, cut at JOB_OUTPUT_MAX.
    char *out;
    size_t out_len, out_cap;
    bool out_cut;
    // Bytes of output already written to the ring: out[0] is byte
    // out_base of everything the job has printed.
    uint64_t out_base;
    // Pending annotations (job_annotate), in order: each is written once
    // the text before its position has been.
    struct job_annot *annot_head, *annot_tail;
    gs_job_t *next;
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

// The pending emulator call: one at a time, since there is one job thread.
typedef struct {
    void (*fn)(void *);
    void *ud;
    uint32_t client;
    bool pending; // posted by the job thread, not yet served
    bool served; // fn has run; the job may still be held for its mode
    bool done; // the job thread may continue
    bool serving; // fn is running right now
    uint32_t defer_token; // the leaf deferred its answer (0: none)
    uint32_t defer_io; // the I/O job answering the deferral (0: none)
    uint32_t defer_req; // the request id of the job the deferred call belongs to
} emu_call_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv_queue = PTHREAD_COND_INITIALIZER; // job thread waits for work
static pthread_cond_t g_cv_call = PTHREAD_COND_INITIALIZER; // job thread waits for its call
static bool g_thread_running;
static gs_job_t *g_queue, *g_queue_tail; // waiting
static gs_job_t *g_active; // on the job thread
static gs_job_t *g_done; // finished, result not yet written
static emu_call_t g_call;
static volatile uint32_t *g_wake_word; // the mailbox's REQ_HEAD (the emulator parks on it)

void job_layer_set_wake_word(volatile uint32_t *word) {
    g_wake_word = word;
}

bool job_thread_running(void) {
    return g_thread_running;
}

static void job_free(gs_job_t *j) {
    while (j->annot_head) {
        struct job_annot *a = j->annot_head;
        j->annot_head = a->next;
        free(a->json);
        free(a);
    }
    free(j->src);
    free(j->out);
    free(j);
}

// --- output ------------------------------------------------------------------

static void job_append_locked(gs_job_t *j, const char *text, size_t len) {
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
    gs_job_t *j = job_current();
    if (!j) {
        // The emulator thread, serving a job's call?
        if (!g_call.serving)
            return false;
        pthread_mutex_lock(&g_mu);
        j = g_active;
        if (j)
            job_append_locked(j, text, len);
        pthread_mutex_unlock(&g_mu);
        return j != NULL;
    }
    pthread_mutex_lock(&g_mu);
    job_append_locked(j, text, len);
    pthread_mutex_unlock(&g_mu);
    if (g_wake_word)
        gs_mailbox_notify(g_wake_word);
    return true;
}

static void wake_emulator(void);

// The escaped length of one byte of output text inside a JSON string.
static size_t escaped_len(unsigned char c) {
    if (c == '"' || c == '\\' || c == '\n' || c == '\t')
        return 2;
    if (c < 0x20)
        return 6;
    return 1;
}

// Writes the job's buffered output as EVT_LOG output records, and its
// annotations at their positions, as far as the ring has room.  Each text
// record is sized by its ESCAPED length so the record fits the ring's record
// bound (gs_mailbox_record_max) -- a 16 KiB chunk of control bytes escapes to
// ~96 KiB, which could never fit the headless ring, and the flush used to
// retry it forever.  A text record never crosses an annotation's position.
// Returns whether everything was written.  g_mu held by the caller for the
// buffer; the ring is the emulator thread's own.
static bool job_flush_output(struct gs_mailbox *m, gs_job_t *j) {
    size_t budget = gs_mailbox_record_max();
    budget = budget > JOB_RECORD_OVERHEAD + 64 ? budget - JOB_RECORD_OVERHEAD : 64;
    for (;;) {
        // An annotation whose position has been reached goes next.
        struct job_annot *a = j->annot_head;
        if (a && a->at <= j->out_base) {
            if (!gs_mailbox_emit_output(m, a->json))
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
        while (n < limit && esc + escaped_len((unsigned char)j->out[n]) <= budget)
            esc += escaped_len((unsigned char)j->out[n++]);
        // Do not split a UTF-8 sequence: back up to a boundary unless this
        // chunk ends where it must (the tail, or an annotation).
        if (n < limit)
            while (n > 1 && ((unsigned char)j->out[n] & 0xC0u) == 0x80u)
                n--;
        if (n == 0)
            n = 1;
        size_t cap = 64 + esc + 8;
        char *json = (char *)malloc(cap);
        if (!json)
            return true; // drop rather than wedge
        size_t o = (size_t)snprintf(json, cap, "{\"event\":\"output\",\"id\":%u,\"client\":%u,\"text\":\"",
                                    (unsigned)j->req_id, (unsigned)j->client);
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)j->out[i];
            if (c == '"' || c == '\\') {
                json[o++] = '\\';
                json[o++] = (char)c;
            } else if (c == '\n') {
                json[o++] = '\\';
                json[o++] = 'n';
            } else if (c == '\t') {
                json[o++] = '\\';
                json[o++] = 't';
            } else if (c < 0x20) {
                o += (size_t)snprintf(json + o, 8, "\\u%04x", c);
            } else {
                json[o++] = (char)c;
            }
        }
        json[o++] = '"';
        json[o++] = '}';
        json[o] = '\0';
        bool ok = gs_mailbox_emit_output(m, json);
        free(json);
        if (!ok)
            return false;
        memmove(j->out, j->out + n, j->out_len - n);
        j->out_len -= n;
        j->out_base += n;
    }
}

// The job an annotation or a line of output belongs to right now: the one
// on this job thread, or the one whose call the emulator thread is serving.
// g_mu held.
static gs_job_t *annotation_target_locked(void) {
    gs_job_t *j = job_current();
    if (j)
        return j;
    return g_call.serving ? g_active : NULL;
}

// The entries of map `m` as JSON object members, without the braces (tagged
// JSON, the bridge's form); "" for none.  Malloc'd, NULL on failure.
static char *annotation_fields(const value_t *m) {
    if (!m || m->kind != V_MAP || m->map.len == 0)
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
    if (!job_current() && !g_call.serving)
        return false;
    // Everything but the job's ids is formatted before the lock.
    vbuf_t kjson = {0};
    value_t kv = val_str(kind ? kind : "");
    value_format(&kv, VFMT_JSON, &kjson);
    value_free(&kv);
    char *bodies[2] = {annotation_fields(fields), reduced ? annotation_fields(reduced) : NULL};
    size_t max = gs_mailbox_record_max();
    max = max > 16 ? max - 16 : max; // the record header
    struct job_annot *a = (struct job_annot *)calloc(1, sizeof(*a));
    bool placed = false, shortened = false;
    pthread_mutex_lock(&g_mu);
    // The job is resolved once: the ids in the record and the stream it joins
    // are the same job's.
    gs_job_t *j = annotation_target_locked();
    // Nothing to attach it to, or past the 1 MiB cut: dropped.
    if (a && kjson.p && bodies[0] && j && !j->out_cut) {
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
    pthread_mutex_unlock(&g_mu);
    if (!placed)
        free(a);
    vbuf_free(&kjson);
    free(bodies[0]);
    free(bodies[1]);
    if (!placed)
        return false;
    if (used_reduced)
        *used_reduced = shortened;
    wake_emulator();
    return true;
}

// The output every job has pending, to the ring.  Called from the drain.
static void jobs_flush_output(struct gs_mailbox *m) {
    pthread_mutex_lock(&g_mu);
    if (g_active)
        job_flush_output(m, g_active);
    for (gs_job_t *j = g_done; j; j = j->next)
        job_flush_output(m, j);
    pthread_mutex_unlock(&g_mu);
}

// Wakes the emulator thread: it parks on REQ_HEAD when the machine is
// stopped, and the platform's notify targets that word.
static void wake_emulator(void) {
    if (g_wake_word)
        gs_mailbox_notify(g_wake_word);
}

// --- the job thread -------------------------------------------------------

static void post_call(void (*fn)(void *ud), void *ud);

static void *job_thread_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_queue)
            pthread_cond_wait(&g_cv_queue, &g_mu);
        gs_job_t *j = g_queue;
        g_queue = j->next;
        if (!g_queue)
            g_queue_tail = NULL;
        j->next = NULL;
        g_active = j;
        pthread_mutex_unlock(&g_mu);

        job_seam_set_current(j, j->client, &j->cancel);
        j->rc = job_glue_run_source(j->src, true);
        job_seam_set_current(NULL, 0, NULL);

        pthread_mutex_lock(&g_mu);
        g_active = NULL;
        j->next = g_done;
        g_done = j;
        pthread_mutex_unlock(&g_mu);
        wake_emulator();
    }
    return NULL;
}

bool job_thread_start(size_t stack_bytes) {
    if (g_thread_running)
        return true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_bytes)
        pthread_attr_setstacksize(&attr, stack_bytes);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int rc = pthread_create(&t, &attr, job_thread_main, NULL);
    pthread_attr_destroy(&attr);
    g_thread_running = rc == 0;
    if (g_thread_running)
        job_seam_set_poster(post_call);
    return g_thread_running;
}

// The poster the seam calls from the job thread: hand fn to the emulator
// thread and wait until it has run (and any mode it started has ended).
static void post_call(void (*fn)(void *ud), void *ud) {
    pthread_mutex_lock(&g_mu);
    g_call.fn = fn;
    g_call.ud = ud;
    g_call.client = job_current_client();
    g_call.served = false;
    g_call.done = false;
    g_call.pending = true;
    pthread_mutex_unlock(&g_mu);
    wake_emulator();
    pthread_mutex_lock(&g_mu);
    while (!g_call.done)
        pthread_cond_wait(&g_cv_call, &g_mu);
    g_call.pending = false;
    pthread_mutex_unlock(&g_mu);
}

bool job_serving_call(void) {
    return g_call.serving;
}

// --- a call's deferred answer (emulator thread) --------------------------------

static uint32_t g_defer_seq;

uint32_t job_call_defer(void) {
    if (!g_call.serving || g_call.defer_token)
        return 0;
    g_defer_seq = (g_defer_seq + 1) & 0x7fffffffu;
    if (!g_defer_seq)
        g_defer_seq = 1;
    g_call.defer_token = g_defer_seq | 0x80000000u;
    g_call.defer_io = 0;
    pthread_mutex_lock(&g_mu);
    g_call.defer_req = g_active ? g_active->req_id : 0;
    pthread_mutex_unlock(&g_mu);
    return g_call.defer_token;
}

void job_call_bind_io(uint32_t token, uint32_t io_job) {
    pthread_mutex_lock(&g_mu);
    if (g_call.defer_token == token)
        g_call.defer_io = io_job;
    pthread_mutex_unlock(&g_mu);
}

uint32_t job_call_request_id(uint32_t token) {
    pthread_mutex_lock(&g_mu);
    uint32_t id = g_call.defer_token == token ? g_call.defer_req : 0;
    pthread_mutex_unlock(&g_mu);
    return id;
}

void job_call_complete(uint32_t token, bool ok, const char *json) {
    pthread_mutex_lock(&g_mu);
    if (g_call.defer_token != token) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    g_call.defer_token = 0;
    g_call.defer_io = 0;
    if (!ok) {
        // The error text out of {"error":"..."}; anything else verbatim.
        char error[256];
        const char *q = json ? strstr(json, "\"error\":\"") : NULL;
        snprintf(error, sizeof error, "%s", q ? q + 9 : (json ? json : "failed"));
        size_t n = strlen(error);
        if (n >= 2 && strcmp(error + n - 2, "\"}") == 0)
            error[n - 2] = '\0';
        job_seam_note_failure(error);
    }
    pthread_mutex_unlock(&g_mu);
}

// --- submission and cancel (any thread) -------------------------------------

bool job_submit_script(uint32_t client, uint32_t req_id, const char *src, size_t len) {
    if (!g_thread_running)
        return false;
    gs_job_t *j = (gs_job_t *)calloc(1, sizeof(*j));
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
    pthread_mutex_lock(&g_mu);
    if (g_queue_tail)
        g_queue_tail->next = j;
    else
        g_queue = j;
    g_queue_tail = j;
    pthread_cond_signal(&g_cv_queue);
    pthread_mutex_unlock(&g_mu);
    return true;
}

static int cancel_matching(uint32_t client, uint32_t req_id, bool any_req) {
    int n = 0;
    pthread_mutex_lock(&g_mu);
    // Queued jobs never start: they finish at once, cancelled.
    gs_job_t **pp = &g_queue;
    while (*pp) {
        gs_job_t *j = *pp;
        if (j->client == client && (any_req || j->req_id == req_id)) {
            *pp = j->next;
            if (g_queue_tail == j)
                g_queue_tail = NULL;
            j->cancel = true;
            j->rc = -1;
            j->next = g_done;
            g_done = j;
            n++;
            continue;
        }
        pp = &j->next;
    }
    if (g_queue && !g_queue_tail) {
        for (gs_job_t *j = g_queue; j; j = j->next)
            g_queue_tail = j;
    }
    uint32_t stop_mode = 0;
    uint32_t io_job = 0;
    if (g_active && g_active->client == client && (any_req || g_active->req_id == req_id)) {
        __atomic_store_n(&g_active->cancel, true, __ATOMIC_RELEASE);
        stop_mode = g_active->mode_id;
        // A deferred I/O call of the job ends with it too.
        if (g_call.pending && g_call.defer_token)
            io_job = g_call.defer_io;
        n++;
    }
    pthread_mutex_unlock(&g_mu);
    // The run the job started ends with it; the client's other runs stay.
    if (stop_mode)
        job_glue_stop_mode(client, stop_mode);
    if (io_job)
        io_worker_cancel(io_job);
    return n;
}

int job_cancel(uint32_t client, uint32_t req_id) {
    return cancel_matching(client, req_id, false);
}

int job_cancel_client(uint32_t client) {
    return cancel_matching(client, 0, true);
}

// --- the emulator thread's side ---------------------------------------------

bool job_layer_has_work(void) {
    if (__atomic_load_n(&g_call.pending, __ATOMIC_ACQUIRE) || __atomic_load_n(&g_done, __ATOMIC_ACQUIRE) != NULL)
        return true;
    gs_job_t *a = __atomic_load_n(&g_active, __ATOMIC_ACQUIRE);
    return a && (__atomic_load_n(&a->out_len, __ATOMIC_ACQUIRE) != 0 ||
                 __atomic_load_n(&a->annot_head, __ATOMIC_ACQUIRE) != NULL);
}

int job_layer_service(struct gs_mailbox *m) {
    int written = 0;
    // 0. Output the jobs printed since the last drain, before anything
    // that might answer them.
    jobs_flush_output(m);
    // 1. A posted call: run it here, as the job's client.
    pthread_mutex_lock(&g_mu);
    bool run_call = g_call.pending && !g_call.served;
    void (*fn)(void *) = g_call.fn;
    void *ud = g_call.ud;
    uint32_t client = g_call.client;
    pthread_mutex_unlock(&g_mu);
    if (run_call) {
        uint32_t mode_before = job_glue_mode_id();
        uint32_t prev_client = m->client;
        m->client = client;
        g_call.serving = true;
        g_call.defer_token = 0;
        fn(ud);
        g_call.serving = false;
        m->client = prev_client;
        jobs_flush_output(m);
        uint32_t mode_after = job_glue_mode_id();
        bool hold = (mode_after != mode_before && job_glue_mode_waits(client)) || g_call.defer_token != 0;
        pthread_mutex_lock(&g_mu);
        if (mode_after != mode_before && g_active)
            g_active->mode_id = mode_after;
        g_call.served = true;
        if (!hold) {
            g_call.done = true;
            pthread_cond_signal(&g_cv_call);
        }
        pthread_mutex_unlock(&g_mu);
    }
    // 2. A call held for its mode or a deferred answer: release it once the
    // mode has ended and the answer has come.
    pthread_mutex_lock(&g_mu);
    bool held = g_call.pending && g_call.served && !g_call.done;
    client = g_call.client;
    bool deferred_open = g_call.defer_token != 0;
    pthread_mutex_unlock(&g_mu);
    if (held && !deferred_open && !job_glue_mode_waits(client)) {
        pthread_mutex_lock(&g_mu);
        g_call.done = true;
        pthread_cond_signal(&g_cv_call);
        pthread_mutex_unlock(&g_mu);
    }
    // 3. Finished jobs: their result is the shell's prompt (what shell.run
    // returned), or an error.
    for (;;) {
        pthread_mutex_lock(&g_mu);
        gs_job_t *j = g_done;
        pthread_mutex_unlock(&g_mu);
        if (!j)
            break;
        bool ok;
        if (j->rc == 0) {
            int rc = m->eval("shell.prompt", NULL, m->out, GS_MBX_RESULT_MAX);
            ok = rc == 0;
        } else {
            snprintf(m->out, GS_MBX_RESULT_MAX, "{\"error\":\"%s\"}", j->cancel ? "cancelled" : "command failed");
            ok = false;
        }
        // Its output first, then the result (a client sees them in order).
        pthread_mutex_lock(&g_mu);
        bool flushed = job_flush_output(m, j);
        pthread_mutex_unlock(&g_mu);
        if (!flushed)
            break;
        if (!gs_mailbox_write_result(m, j->req_id, ok, m->out, (uint32_t)strlen(m->out), NULL, 0))
            break; // no room: keep it for the next drain
        written++;
        pthread_mutex_lock(&g_mu);
        g_done = j->next;
        pthread_mutex_unlock(&g_mu);
        job_free(j);
    }
    return written;
}

// --- inline mode -----------------------------------------------------------------

static void (*g_inline_frame)(void);

void job_inline_enable(void (*run_frame)(void)) {
    g_inline_frame = run_frame;
}

bool job_inline_enabled(void) {
    return g_inline_frame != NULL;
}

void job_inline_after_call(uint32_t mode_before) {
    if (!g_inline_frame || g_thread_running)
        return;
    uint32_t client = gs_mailbox_serving_client();
    if (!client || job_glue_mode_id() == mode_before)
        return;
    // Frames until the mode ends -- what holding the call does in threaded
    // mode.  The drain is not re-entered (the request being served is not
    // yet consumed); the I/O worker's completions are, since a leaf may
    // wait on one.
    while (job_glue_mode_waits(client)) {
        g_inline_frame();
        io_worker_service();
    }
}
