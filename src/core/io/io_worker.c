// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// io_worker.c -- see io_worker.h.

#include "io_worker.h"

#include "work_queue.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct io_job {
    wq_item_t link; // first: the worker's queue (work_queue.h)
    uint32_t id;
    io_work_fn work; // a function job, else a publish
    void *work_ud;
    const uint8_t *buf;
    size_t len;
    char *tmp_path;
    char *final_path;
    io_done_fn done;
    void *ud;
    // Observers (emulator thread).
    io_progress_fn progress;
    io_note_fn note;
    void *observer_ud;
    // Progress: the latest values the work reported, and whether the
    // observer has seen them (g_wq.mu).
    uint64_t done_n, total_n;
    bool progress_dirty;
    // Notes in flight, oldest first (g_wq.mu).
    char notes[4][IO_NOTE_MAX];
    int n_notes;
    // Cancel (set by the emulator thread, read by the work) and the last
    // acknowledged tag (g_wq.mu, g_cv_ack).
    bool cancel;
    uint32_t acked_tag;
    bool acked;
    // Filled by the worker.
    bool ok;
    double ms;
    char error[160];
} io_job_t;

// The job whose work runs on this thread (the worker, or the caller of
// io_run_inline), for io_progress / io_cancelled / io_note.
static _Thread_local io_job_t *t_work;
static pthread_cond_t g_cv_ack = PTHREAD_COND_INITIALIZER;

static void io_run(work_queue_t *q, wq_item_t *item);

// The worker: jobs waiting, the one running, and those finished whose done
// callback has not run, in the order they ran.  Its mutex also guards the
// jobs' progress, notes and acks (g_cv_ack) and g_reports.
static work_queue_t g_wq = WORK_QUEUE_INIT(io_run, NULL);
static uint32_t g_next_id = 1;
static uint32_t g_jobs_done;
static double g_last_ms;
static void (*g_waker)(void);

double io_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

int io_write_publish(const uint8_t *buf, size_t len, const char *tmp_path, const char *final_path, char *err,
                     size_t err_cap) {
    if (err && err_cap)
        err[0] = '\0';
    unlink(tmp_path);
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        int e = errno ? errno : EIO;
        if (err)
            snprintf(err, err_cap, "cannot create '%s': %s", tmp_path, strerror(e));
        return -e;
    }
    size_t chunk = (size_t)GS_IO_CHUNK_KB * 1024u;
    for (size_t off = 0; off < len;) {
        size_t n = len - off < chunk ? len - off : chunk;
        if (fwrite(buf + off, 1, n, f) != n) {
            int e = errno ? errno : EIO;
            if (err)
                snprintf(err, err_cap, "write failed at %zu of %zu: %s", off, len, strerror(e));
            fclose(f);
            unlink(tmp_path);
            return -e;
        }
        off += n;
        // Let the filesystem's proxy serve someone else between chunks.
        if (off < len)
            sched_yield();
    }
    if (fclose(f) != 0) {
        int e = errno ? errno : EIO;
        if (err)
            snprintf(err, err_cap, "close failed: %s", strerror(e));
        unlink(tmp_path);
        return -e;
    }
    if (rename(tmp_path, final_path) != 0) {
        int e = errno ? errno : EIO;
        if (err)
            snprintf(err, err_cap, "rename to '%s' failed: %s", final_path, strerror(e));
        unlink(tmp_path);
        return -e;
    }
    return 0;
}

static void enqueue(io_job_t *j);

static void job_free(io_job_t *j) {
    free(j->tmp_path);
    free(j->final_path);
    free(j);
}

// The job whose work the worker runs now (g_wq.mu, or a racy peek).
static inline io_job_t *io_active(void) {
    return (io_job_t *)__atomic_load_n(&g_wq.active, __ATOMIC_ACQUIRE);
}

// One job, on the worker (work_queue.h runs them in order, files each as
// done, then calls the waker).
static void io_run(work_queue_t *q, wq_item_t *item) {
    (void)q;
    io_job_t *j = (io_job_t *)item;
    double t0 = io_now_ms();
    int rc;
    if (__atomic_load_n(&j->cancel, __ATOMIC_ACQUIRE)) {
        snprintf(j->error, sizeof j->error, "cancelled");
        rc = -ECANCELED;
    } else {
        t_work = j;
        rc = j->work ? j->work(j->work_ud, j->error, sizeof j->error)
                     : io_write_publish(j->buf, j->len, j->tmp_path, j->final_path, j->error, sizeof j->error);
        t_work = NULL;
        if (rc == -ECANCELED && !j->error[0])
            snprintf(j->error, sizeof j->error, "cancelled");
    }
    j->ok = rc == 0;
    j->ms = io_now_ms() - t0;
}

bool io_worker_start(size_t stack_bytes) {
    return wq_start(&g_wq, stack_bytes);
}

bool io_worker_running(void) {
    return g_wq.running;
}

void io_worker_set_waker(void (*wake)(void)) {
    g_waker = wake;
    g_wq.wake = wake; // also after each completion
}

uint32_t io_submit_publish(const uint8_t *buf, size_t len, const char *tmp_path, const char *final_path,
                           io_done_fn done, void *ud) {
    if (!g_wq.running)
        return 0;
    io_job_t *j = (io_job_t *)calloc(1, sizeof(*j));
    if (!j)
        return 0;
    j->tmp_path = strdup(tmp_path);
    j->final_path = strdup(final_path);
    if (!j->tmp_path || !j->final_path) {
        job_free(j);
        return 0;
    }
    j->buf = buf;
    j->len = len;
    j->done = done;
    j->ud = ud;
    enqueue(j);
    return j->id;
}

static void enqueue(io_job_t *j) {
    wq_lock(&g_wq);
    j->id = g_next_id++;
    if (g_next_id == 0)
        g_next_id = 1;
    wq_unlock(&g_wq);
    wq_push(&g_wq, &j->link);
}

uint32_t io_submit_work(io_work_fn work, void *ud, io_done_fn done, void *dud) {
    if (!g_wq.running || !work)
        return 0;
    io_job_t *j = (io_job_t *)calloc(1, sizeof(*j));
    if (!j)
        return 0;
    j->work = work;
    j->work_ud = ud;
    j->done = done;
    j->ud = dud;
    enqueue(j);
    return j->id;
}

uint32_t io_submit_job(const io_job_desc_t *d) {
    if (!g_wq.running || !d || !d->work)
        return 0;
    io_job_t *j = (io_job_t *)calloc(1, sizeof(*j));
    if (!j)
        return 0;
    j->work = d->work;
    j->work_ud = d->work_ud;
    j->done = d->done;
    j->ud = d->done_ud;
    j->progress = d->progress;
    j->note = d->note;
    j->observer_ud = d->observer_ud;
    enqueue(j);
    return j->id;
}

// Progress and notes are reported through the worker's wake, so the
// emulator thread services them at its next drain.
static bool g_reports; // progress or notes waiting (g_wq.mu)

void io_progress(uint64_t done, uint64_t total) {
    io_job_t *j = t_work;
    if (!j)
        return;
    if (!g_wq.running || j != io_active()) {
        // Inline: straight to the observer, on this thread.
        if (j->progress)
            j->progress(done, total, j->observer_ud);
        return;
    }
    pthread_mutex_lock(&g_wq.mu);
    j->done_n = done;
    j->total_n = total;
    j->progress_dirty = true;
    g_reports = true;
    pthread_mutex_unlock(&g_wq.mu);
    if (g_waker)
        g_waker();
}

bool io_cancelled(void) {
    io_job_t *j = t_work;
    return j && __atomic_load_n(&j->cancel, __ATOMIC_ACQUIRE);
}

bool io_note(const char *json) {
    io_job_t *j = t_work;
    if (!j || !json || strlen(json) >= IO_NOTE_MAX)
        return false;
    if (!g_wq.running || j != io_active()) {
        if (j->note)
            j->note(json, j->observer_ud);
        return true;
    }
    pthread_mutex_lock(&g_wq.mu);
    if (j->n_notes >= (int)(sizeof j->notes / sizeof j->notes[0])) {
        pthread_mutex_unlock(&g_wq.mu);
        return false;
    }
    snprintf(j->notes[j->n_notes++], IO_NOTE_MAX, "%s", json);
    g_reports = true;
    pthread_mutex_unlock(&g_wq.mu);
    if (g_waker)
        g_waker();
    return true;
}

int io_wait_ack(uint32_t tag, unsigned ms) {
    io_job_t *j = t_work;
    if (!j)
        return -ETIMEDOUT;
    if (!g_wq.running || j != io_active())
        return -ETIMEDOUT; // inline: nobody can answer while we hold the thread
    struct timeval now;
    gettimeofday(&now, NULL);
    struct timespec until;
    until.tv_sec = now.tv_sec + (time_t)(ms / 1000u);
    until.tv_nsec = (long)now.tv_usec * 1000L + (long)(ms % 1000u) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    int rc = 0;
    pthread_mutex_lock(&g_wq.mu);
    for (;;) {
        if (j->acked && (tag == 0 || j->acked_tag == tag)) {
            j->acked = false;
            break;
        }
        if (__atomic_load_n(&j->cancel, __ATOMIC_ACQUIRE)) {
            rc = -ECANCELED;
            break;
        }
        if (pthread_cond_timedwait(&g_cv_ack, &g_wq.mu, &until) == ETIMEDOUT) {
            if (!(j->acked && (tag == 0 || j->acked_tag == tag)))
                rc = -ETIMEDOUT;
            else
                j->acked = false;
            break;
        }
    }
    pthread_mutex_unlock(&g_wq.mu);
    return rc;
}

bool io_worker_cancel(uint32_t id) {
    bool found = false;
    pthread_mutex_lock(&g_wq.mu);
    for (wq_item_t *it = g_wq.head; it; it = it->next) {
        io_job_t *j = (io_job_t *)it;
        if (j->id == id) {
            __atomic_store_n(&j->cancel, true, __ATOMIC_RELEASE);
            found = true;
        }
    }
    io_job_t *active = io_active();
    if (active && active->id == id) {
        __atomic_store_n(&active->cancel, true, __ATOMIC_RELEASE);
        found = true;
    }
    pthread_cond_broadcast(&g_cv_ack);
    pthread_mutex_unlock(&g_wq.mu);
    return found;
}

bool io_worker_ack(uint32_t id, uint32_t tag) {
    pthread_mutex_lock(&g_wq.mu);
    io_job_t *j = io_active() && io_active()->id == id ? io_active() : NULL;
    if (j) {
        j->acked = true;
        j->acked_tag = tag;
        pthread_cond_broadcast(&g_cv_ack);
    }
    pthread_mutex_unlock(&g_wq.mu);
    return j != NULL;
}

int io_run_inline(const io_job_desc_t *d, char *err, size_t err_cap) {
    if (!d || !d->work)
        return -EINVAL;
    io_job_t j;
    memset(&j, 0, sizeof j);
    j.progress = d->progress;
    j.note = d->note;
    j.observer_ud = d->observer_ud;
    io_job_t *prev = t_work;
    t_work = &j;
    int rc = d->work(d->work_ud, err, err_cap);
    t_work = prev;
    return rc;
}

// Delivers the active job's progress and notes to its observers.
static void service_reports(void) {
    for (;;) {
        pthread_mutex_lock(&g_wq.mu);
        if (!g_reports) {
            pthread_mutex_unlock(&g_wq.mu);
            return;
        }
        io_job_t *j = io_active();
        // A job that finished before its reports were seen: its records
        // are on the done list, whose progress/notes are delivered by the
        // completion path below (service_job_reports).
        if (!j) {
            g_reports = false;
            pthread_mutex_unlock(&g_wq.mu);
            return;
        }
        bool prog = j->progress_dirty;
        uint64_t dn = j->done_n, tn = j->total_n;
        j->progress_dirty = false;
        char note[IO_NOTE_MAX];
        bool has_note = j->n_notes > 0;
        if (has_note) {
            memcpy(note, j->notes[0], IO_NOTE_MAX);
            memmove(j->notes[0], j->notes[1], sizeof j->notes[0] * (size_t)(j->n_notes - 1));
            j->n_notes--;
        }
        g_reports = j->n_notes > 0;
        io_progress_fn pf = j->progress;
        io_note_fn nf = j->note;
        void *oud = j->observer_ud;
        pthread_mutex_unlock(&g_wq.mu);
        if (prog && pf)
            pf(dn, tn, oud);
        if (has_note && nf)
            nf(note, oud);
        if (!has_note)
            return;
    }
}

// The reports a finished job left undelivered, in order, before its done.
static void service_job_reports(io_job_t *j) {
    if (j->progress_dirty && j->progress)
        j->progress(j->done_n, j->total_n, j->observer_ud);
    j->progress_dirty = false;
    for (int i = 0; i < j->n_notes; i++)
        if (j->note)
            j->note(j->notes[i], j->observer_ud);
    j->n_notes = 0;
}

bool io_worker_has_work(void) {
    return __atomic_load_n(&g_wq.done, __ATOMIC_ACQUIRE) != NULL || __atomic_load_n(&g_reports, __ATOMIC_ACQUIRE);
}

bool io_worker_busy(void) {
    wq_lock(&g_wq);
    bool busy = g_wq.head || g_wq.active || g_wq.done;
    wq_unlock(&g_wq);
    return busy;
}

int io_worker_service(void) {
    int n = 0;
    service_reports();
    for (;;) {
        io_job_t *j = (io_job_t *)wq_done_pop(&g_wq);
        if (!j)
            break;
        g_jobs_done++;
        g_last_ms = j->ms;
        service_job_reports(j);
        if (j->done)
            j->done(j->ok, j->ms, j->ok ? NULL : j->error, j->ud);
        job_free(j);
        n++;
    }
    return n;
}

void io_worker_wait_idle(void) {
    for (;;) {
        io_worker_service();
        wq_lock(&g_wq);
        bool busy = g_wq.head || g_wq.active || g_wq.done;
        wq_unlock(&g_wq);
        if (!busy)
            return;
        usleep(500);
    }
}

uint32_t io_worker_jobs_done(void) {
    return g_jobs_done;
}

double io_worker_last_ms(void) {
    return g_last_ms;
}
