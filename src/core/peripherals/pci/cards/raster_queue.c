// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// raster_queue.c
// The 3D rasterisers' draw-state ring and worker-thread command ring (see
// raster_queue.h).

#include "raster_queue.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// Constants and Macros
// ============================================================================

// How long a dry worker spins before it sleeps.  The commands are small and
// arrive in bursts (a triangle every few microseconds of producer time, a
// frame's objects while the card walks its control program), so a worker
// that slept on every empty poll would pay a futex wake and a context switch
// per command -- measured on the Voodoo2: the threaded row took LONGER than
// the synchronous one.  A short spin catches the next command in the common
// case; the sleep path is for the genuinely idle stretches.
#define RQ_WORKER_SPIN 4096

// ============================================================================
// Static Helpers
// ============================================================================

#if GS_RASTER_THREAD_BACKEND
// Monotonic nanoseconds, for the stats.
static uint64_t rq_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// One iteration of a busy-wait: a pause hint where the ISA has one.
static inline void rq_cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

// The worker: drain the ring, execute in order, retire, sleep when dry.
static void *rq_worker_main(void *arg) {
    rq_t *q = (rq_t *)arg;
    for (;;) {
        uint64_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
        uint64_t h = atomic_load_explicit(&q->head, memory_order_acquire);
        for (int spin = 0; h == t && spin < RQ_WORKER_SPIN; spin++) {
            rq_cpu_relax();
            h = atomic_load_explicit(&q->head, memory_order_acquire);
        }
        if (h == t) {
            q->stats.n_worker_sleep++;
            if (q->idle)
                q->idle(q->exec_ctx);
            pthread_mutex_lock(&q->mu);
            atomic_store_explicit(&q->worker_idle, 1, memory_order_seq_cst);
            // Re-check under the lock: a producer that stored head after
            // our first load either sees worker_idle and signals, or its
            // store is visible here (Dekker on head/worker_idle).
            while (atomic_load_explicit(&q->head, memory_order_seq_cst) == t && !q->stop)
                pthread_cond_wait(&q->cv_work, &q->mu);
            atomic_store_explicit(&q->worker_idle, 0, memory_order_seq_cst);
            bool stop = q->stop && atomic_load_explicit(&q->head, memory_order_seq_cst) == t;
            pthread_mutex_unlock(&q->mu);
            if (stop) {
                struct timespec ts = {0, 0}; // stays zero where the clock is unsupported (wasm)
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
                q->stats.worker_cpu_s = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
                return NULL;
            }
            continue;
        }
        q->exec(q->exec_ctx, q->q + (size_t)(t & (q->depth - 1u)) * q->cmd_size);
        atomic_store_explicit(&q->tail, t + 1u, memory_order_seq_cst);
        // Wake the producer only when it announced it is waiting
        if (atomic_load_explicit(&q->producer_waiting, memory_order_seq_cst)) {
            pthread_mutex_lock(&q->mu);
            pthread_cond_broadcast(&q->cv_done);
            pthread_mutex_unlock(&q->mu);
        }
    }
}
#endif

// ============================================================================
// Lifecycle
// ============================================================================

bool rq_init(rq_t *q, size_t state_size, uint32_t state_slots, rq_build_fn build, void *build_ctx) {
    memset(q, 0, sizeof(*q));
    q->st = (uint8_t *)calloc(state_slots, state_size);
    q->st_seq = (uint64_t *)calloc(state_slots, sizeof(uint64_t));
    if (!q->st || !q->st_seq) {
        free(q->st);
        free(q->st_seq);
        q->st = NULL;
        q->st_seq = NULL;
        return false;
    }
    q->st_size = state_size;
    q->st_slots = state_slots;
    q->build = build;
    q->build_ctx = build_ctx;
    q->st_dirty = true; // the first command builds its state
    return true;
}

bool rq_start_thread(rq_t *q, size_t cmd_size, uint32_t depth, rq_exec_fn exec, rq_idle_fn idle, void *ctx) {
#if GS_RASTER_THREAD_BACKEND
    q->q = (uint8_t *)calloc(depth, cmd_size);
    if (!q->q)
        return false;
    q->cmd_size = cmd_size;
    q->depth = depth;
    q->exec = exec;
    q->idle = idle;
    q->exec_ctx = ctx;
    atomic_store(&q->head, 0);
    atomic_store(&q->tail, 0);
    atomic_store(&q->worker_idle, 0);
    atomic_store(&q->producer_waiting, 0);
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv_work, NULL);
    pthread_cond_init(&q->cv_done, NULL);
    q->stop = false;
    if (pthread_create(&q->thr, NULL, rq_worker_main, q) != 0) {
        pthread_mutex_destroy(&q->mu);
        pthread_cond_destroy(&q->cv_work);
        pthread_cond_destroy(&q->cv_done);
        free(q->q);
        q->q = NULL;
        return false;
    }
    q->threaded = true;
    return true;
#else
    (void)q;
    (void)cmd_size;
    (void)depth;
    (void)exec;
    (void)idle;
    (void)ctx;
    return false;
#endif
}

void rq_fini(rq_t *q, rq_stats_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
#if GS_RASTER_THREAD_BACKEND
    if (q->threaded) {
        // Drain, then tell the (by now idle) worker to leave
        rq_wait_tail(q, atomic_load(&q->head));
        pthread_mutex_lock(&q->mu);
        q->stop = true;
        pthread_cond_broadcast(&q->cv_work);
        pthread_mutex_unlock(&q->mu);
        pthread_join(q->thr, NULL);
        if (out)
            *out = q->stats;
        pthread_mutex_destroy(&q->mu);
        pthread_cond_destroy(&q->cv_work);
        pthread_cond_destroy(&q->cv_done);
        free(q->q);
        q->q = NULL;
        q->threaded = false;
    }
#endif
    free(q->st);
    free(q->st_seq);
    q->st = NULL;
    q->st_seq = NULL;
}

// ============================================================================
// Operations (Public API)
// ============================================================================

void rq_wait_tail(rq_t *q, uint64_t want) {
#if GS_RASTER_THREAD_BACKEND
    if (atomic_load_explicit(&q->tail, memory_order_acquire) >= want)
        return;
    uint64_t t0 = rq_now_ns();
    pthread_mutex_lock(&q->mu);
    atomic_store_explicit(&q->producer_waiting, 1, memory_order_seq_cst);
    while (atomic_load_explicit(&q->tail, memory_order_seq_cst) < want)
        pthread_cond_wait(&q->cv_done, &q->mu);
    atomic_store_explicit(&q->producer_waiting, 0, memory_order_seq_cst);
    pthread_mutex_unlock(&q->mu);
    q->stats.wait_ns += rq_now_ns() - t0;
#else
    (void)q;
    (void)want;
#endif
}

void rq_state_advance(rq_t *q) {
    uint32_t next = (q->st_cur + 1u) & (q->st_slots - 1u);
#if GS_RASTER_THREAD_BACKEND
    // The slot may still be read by a queued command: wait for it to retire
    if (q->threaded && q->st_seq[next]) {
        if (atomic_load_explicit(&q->tail, memory_order_acquire) < q->st_seq[next])
            q->stats.n_state_wait++;
        rq_wait_tail(q, q->st_seq[next]);
    }
#endif
    q->build(q->build_ctx, rq_state(q, next));
    q->st_cur = next;
    q->st_dirty = false;
}

void rq_sync(rq_t *q) {
#if GS_RASTER_THREAD_BACKEND
    if (q->threaded) {
        uint64_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
        q->stats.n_sync++;
        if (atomic_load_explicit(&q->tail, memory_order_acquire) < h)
            q->stats.n_sync_wait++;
        rq_wait_tail(q, h);
    }
#else
    (void)q;
#endif
}
