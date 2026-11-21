// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// work_queue.h
// One pthread FIFO worker: a thread that takes items off a queue in order,
// runs each with the lock released, and files it on a done list in the
// order it finished, then wakes whoever reports completions.  The shell's
// job thread (job/job.c) and the I/O worker (io/io_worker.c) are two of
// these; each keeps its own item type -- with a wq_item_t as its first
// member -- and its own semantics (cancel, progress, the job seam), and may
// keep more of its state under the queue's mutex.

#ifndef WORK_QUEUE_H
#define WORK_QUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

// The link every queued item starts with.
typedef struct wq_item {
    struct wq_item *next;
} wq_item_t;

typedef struct work_queue work_queue_t;

// Runs one item on the worker, with the queue's mutex released.
typedef void (*wq_run_fn)(work_queue_t *q, wq_item_t *item);

// A worker and its lists.  `mu` guards head/tail, active and done; the
// owner may guard more of its own state with it (wq_lock).
struct work_queue {
    pthread_mutex_t mu;
    pthread_cond_t cv; // the worker waits here for work
    wq_item_t *head, *tail; // waiting, oldest first
    wq_item_t *active; // on the worker right now
    wq_item_t *done, *done_tail; // finished, oldest first, until the owner takes them
    wq_run_fn run;
    void (*wake)(void); // called after an item is filed as done (may be NULL)
    bool running; // the thread exists
};

// A static initialiser: `run` runs each item, `wake` reports a completion.
#define WORK_QUEUE_INIT(run_fn, wake_fn)                                                                               \
    {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .run = (run_fn), .wake = (wake_fn)}

// Start the worker thread (detached) with `stack_bytes` of stack (0: the
// default); true when it runs, also when it already did.
bool wq_start(work_queue_t *q, size_t stack_bytes);

// Append `item` to the waiting queue and wake the worker.
void wq_push(work_queue_t *q, wq_item_t *item);

// The queue's mutex, for the owner's walks over the lists and its own state.
void wq_lock(work_queue_t *q);
void wq_unlock(work_queue_t *q);

// With the lock held: file `item` on the done list, after those already there.
void wq_done_append_locked(work_queue_t *q, wq_item_t *item);

// With the lock held: move every waiting item `match` accepts to the done
// list, in queue order, calling `on_move` on each as it goes.  Returns how
// many moved.
int wq_cancel_waiting_locked(work_queue_t *q, bool (*match)(const wq_item_t *item, void *ctx),
                             void (*on_move)(wq_item_t *item, void *ctx), void *ctx);

// The oldest finished item, left on the list (NULL: none).  Takes the lock.
wq_item_t *wq_done_peek(work_queue_t *q);

// Remove the oldest finished item (the one wq_done_peek returned) and
// return it.  Takes the lock.
wq_item_t *wq_done_pop(work_queue_t *q);

#endif // WORK_QUEUE_H
