// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// job.c -- see job.h.

#include "job.h"

#include "mailbox/mailbox.h"

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
    gs_job_t *next;
};

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
    free(j->src);
    free(j);
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
    return g_call.defer_token;
}

void job_call_complete(uint32_t token, bool ok, const char *json) {
    pthread_mutex_lock(&g_mu);
    if (g_call.defer_token != token) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    g_call.defer_token = 0;
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
    if (g_active && g_active->client == client && (any_req || g_active->req_id == req_id)) {
        __atomic_store_n(&g_active->cancel, true, __ATOMIC_RELEASE);
        stop_mode = g_active->mode_id;
        n++;
    }
    pthread_mutex_unlock(&g_mu);
    // The run the job started ends with it; the client's other runs stay.
    if (stop_mode)
        job_glue_stop_mode(client, stop_mode);
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
    return __atomic_load_n(&g_call.pending, __ATOMIC_ACQUIRE) || __atomic_load_n(&g_done, __ATOMIC_ACQUIRE) != NULL;
}

int job_layer_service(struct gs_mailbox *m) {
    int written = 0;
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
        if (!gs_mailbox_write_result(m, j->req_id, ok, m->out, (uint32_t)strlen(m->out)))
            break; // no room: keep it for the next drain
        written++;
        pthread_mutex_lock(&g_mu);
        g_done = j->next;
        pthread_mutex_unlock(&g_mu);
        job_free(j);
    }
    return written;
}
