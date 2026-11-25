// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// raster_queue.h
// The command queue the PCI 3D rasterisers share (the QuickDraw 3D card's
// qd3d_raster.c, the Voodoo2's voodoo2_raster.c): a draw-state ring the
// producer fills only when the registers changed, and, for the thread
// backend, a bounded single-producer/single-consumer command ring drained
// by one worker pthread that executes each command to completion in order.
//
// `head` counts commands submitted, `tail` commands RETIRED: head == tail is
// "everything observed", head - tail == depth is "full".  Both sides sleep on
// condition variables only when they have announced they are about to (the
// idle/waiting flags), so the steady state -- worker busy, producer ahead --
// makes no system calls at all.  A draw-state slot is rewritten only once
// every command that referenced it has retired.
//
// The thread backend builds everywhere the core builds: native pthreads,
// and Emscripten's pthreads-on-Web-Workers, created on demand at the card's
// first boot (no worker pool: a pool made the browser's startup ready gate
// flaky in CI).  A build can leave it out with -DGS_RASTER_THREAD_BACKEND=0
// (EXTRA_CFLAGS): no pthread code is compiled, rq_start_thread fails, and
// each card's raster=thread falls back to its synchronous walker with a log
// line (and its default backend becomes the walker).
//
// Each card keeps its backend enum, its executor, the synchronous dispatch
// and anything card-specific (the Voodoo2's GPU face) -- the queue knows a
// command only as `cmd_size` bytes and a state slot only as `state_size`.

#ifndef RASTER_QUEUE_H
#define RASTER_QUEUE_H

// === Includes ===

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// === Constants ===

#ifndef GS_RASTER_THREAD_BACKEND
#define GS_RASTER_THREAD_BACKEND 1
#endif

#if GS_RASTER_THREAD_BACKEND
#include <pthread.h>
#include <stdatomic.h>
#endif

// === Type Definitions ===

// Fills a fresh draw-state slot from the card's live registers.
typedef void (*rq_build_fn)(void *ctx, void *state);

// Executes one queued command on the worker (the card finds the command's
// state slot with rq_state).
typedef void (*rq_exec_fn)(void *ctx, const void *cmd);

// Runs on the worker just before it sleeps (the Voodoo2's translator
// publishes its open draw range); may be NULL.
typedef void (*rq_idle_fn)(void *ctx);

// The thread backend's decomposition, for a card's stats line: how many
// commands, how often the producer fenced and how long it waited, how often
// the worker went to sleep, and the worker's own CPU time.
typedef struct rq_stats {
    uint64_t n_submit, n_sync, n_sync_wait, n_room_wait, n_state_wait, n_worker_sleep;
    uint64_t wait_ns;
    double worker_cpu_s;
} rq_stats_t;

// One card's queue.  Visible so the per-command steps inline; the fields are
// the queue's own.
typedef struct rq {
    // The draw-state ring: st_seq holds 1 + the sequence number of the last
    // command that referenced the slot (0 = never used).
    uint8_t *st;
    size_t st_size;
    uint32_t st_slots;
    uint64_t *st_seq;
    uint32_t st_cur;
    bool st_dirty;
    rq_build_fn build;
    void *build_ctx;
    bool threaded; // the worker is running
#if GS_RASTER_THREAD_BACKEND
    uint8_t *q;
    size_t cmd_size;
    uint32_t depth;
    _Atomic uint64_t head, tail;
    _Atomic int worker_idle; // the worker is (about to be) asleep
    _Atomic int producer_waiting; // the producer is (about to be) asleep
    pthread_mutex_t mu;
    pthread_cond_t cv_work, cv_done;
    bool stop;
    pthread_t thr;
    rq_exec_fn exec;
    rq_idle_fn idle;
    void *exec_ctx;
    rq_stats_t stats;
#endif
} rq_t;

// === Lifecycle ===

// Allocates the draw-state ring (`state_slots` a power of two) and marks it
// dirty, so the first command builds a state.  False when out of memory.
bool rq_init(rq_t *q, size_t state_size, uint32_t state_slots, rq_build_fn build, void *build_ctx);

// Starts the worker over a `depth`-command ring (a power of two).  False --
// and the queue stays synchronous -- when the build has no thread backend or
// the thread could not be created.
bool rq_start_thread(rq_t *q, size_t cmd_size, uint32_t depth, rq_exec_fn exec, rq_idle_fn idle, void *ctx);

// Drains and joins the worker (if any), copies its stats to `out` (may be
// NULL; zeroed for a synchronous queue) and frees the rings.
void rq_fini(rq_t *q, rq_stats_t *out);

// === Operations ===

// Producer: wait until `tail` reaches at least `want` (out of line; the
// steady state never calls it).
void rq_wait_tail(rq_t *q, uint64_t want);

// Producer: take the next state slot, once everything that read it retired,
// and build it.  Out of line: it runs only when the registers changed.
void rq_state_advance(rq_t *q);

// The registers changed: the next command builds a new state slot.
static inline void rq_state_dirty(rq_t *q) {
    q->st_dirty = true;
}

// Draw-state slot `slot`.
static inline void *rq_state(const rq_t *q, uint32_t slot) {
    return q->st + (size_t)slot * q->st_size;
}

// Producer, once per command: make the current state slot hold the live
// registers and charge it to this command.  Returns the slot the command
// carries.
static inline uint32_t rq_begin(rq_t *q) {
    uint64_t seq = 0;
#if GS_RASTER_THREAD_BACKEND
    if (q->threaded)
        seq = atomic_load_explicit(&q->head, memory_order_relaxed);
#endif
    if (q->st_dirty)
        rq_state_advance(q);
    q->st_seq[q->st_cur] = seq + 1u;
    return q->st_cur;
}

// Producer: queue `cmd` (cmd_size bytes) for the worker.  Threaded queues
// only; waits for room when the ring is full.
static inline void rq_enqueue(rq_t *q, const void *cmd) {
#if GS_RASTER_THREAD_BACKEND
    uint64_t seq = atomic_load_explicit(&q->head, memory_order_relaxed);
    q->stats.n_submit++;
    // Room: the slot about to be written must have been retired
    if (seq >= q->depth) {
        if (atomic_load_explicit(&q->tail, memory_order_acquire) < seq + 1u - q->depth)
            q->stats.n_room_wait++;
        rq_wait_tail(q, seq + 1u - q->depth);
    }
    memcpy(q->q + (size_t)(seq & (q->depth - 1u)) * q->cmd_size, cmd, q->cmd_size);
    atomic_store_explicit(&q->head, seq + 1u, memory_order_seq_cst);
    // Wake the worker only when it announced it is going to sleep
    if (atomic_load_explicit(&q->worker_idle, memory_order_seq_cst)) {
        pthread_mutex_lock(&q->mu);
        pthread_cond_signal(&q->cv_work);
        pthread_mutex_unlock(&q->mu);
    }
#else
    (void)q;
    (void)cmd;
#endif
}

// Producer: wait until every queued command has retired (a no-op for a
// synchronous queue).
void rq_sync(rq_t *q);

#endif // RASTER_QUEUE_H
