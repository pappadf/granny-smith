// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// io_worker.c -- see io_worker.h.

#include "io_worker.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct io_job {
    uint32_t id;
    const uint8_t *buf;
    size_t len;
    char *tmp_path;
    char *final_path;
    io_done_fn done;
    void *ud;
    // Filled by the worker.
    bool ok;
    double ms;
    char error[160];
    struct io_job *next;
} io_job_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static bool g_running;
static io_job_t *g_queue, *g_queue_tail;
static io_job_t *g_active;
static io_job_t *g_done;
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

static void job_free(io_job_t *j) {
    free(j->tmp_path);
    free(j->final_path);
    free(j);
}

static void *worker_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_queue)
            pthread_cond_wait(&g_cv, &g_mu);
        io_job_t *j = g_queue;
        g_queue = j->next;
        if (!g_queue)
            g_queue_tail = NULL;
        j->next = NULL;
        g_active = j;
        pthread_mutex_unlock(&g_mu);

        double t0 = io_now_ms();
        int rc = io_write_publish(j->buf, j->len, j->tmp_path, j->final_path, j->error, sizeof j->error);
        j->ok = rc == 0;
        j->ms = io_now_ms() - t0;

        pthread_mutex_lock(&g_mu);
        g_active = NULL;
        // Append, so completions are reported in the order the jobs ran.
        io_job_t **pp = &g_done;
        while (*pp)
            pp = &(*pp)->next;
        *pp = j;
        pthread_mutex_unlock(&g_mu);
        if (g_waker)
            g_waker();
    }
    return NULL;
}

bool io_worker_start(size_t stack_bytes) {
    if (g_running)
        return true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_bytes)
        pthread_attr_setstacksize(&attr, stack_bytes);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int rc = pthread_create(&t, &attr, worker_main, NULL);
    pthread_attr_destroy(&attr);
    g_running = rc == 0;
    return g_running;
}

bool io_worker_running(void) {
    return g_running;
}

void io_worker_set_waker(void (*wake)(void)) {
    g_waker = wake;
}

uint32_t io_submit_publish(const uint8_t *buf, size_t len, const char *tmp_path, const char *final_path,
                           io_done_fn done, void *ud) {
    if (!g_running)
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
    pthread_mutex_lock(&g_mu);
    j->id = g_next_id++;
    if (g_next_id == 0)
        g_next_id = 1;
    if (g_queue_tail)
        g_queue_tail->next = j;
    else
        g_queue = j;
    g_queue_tail = j;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return j->id;
}

bool io_worker_has_work(void) {
    return __atomic_load_n(&g_done, __ATOMIC_ACQUIRE) != NULL;
}

bool io_worker_busy(void) {
    pthread_mutex_lock(&g_mu);
    bool busy = g_queue || g_active || g_done;
    pthread_mutex_unlock(&g_mu);
    return busy;
}

int io_worker_service(void) {
    int n = 0;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        io_job_t *j = g_done;
        if (j)
            g_done = j->next;
        pthread_mutex_unlock(&g_mu);
        if (!j)
            break;
        g_jobs_done++;
        g_last_ms = j->ms;
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
        pthread_mutex_lock(&g_mu);
        bool busy = g_queue || g_active || g_done;
        pthread_mutex_unlock(&g_mu);
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
