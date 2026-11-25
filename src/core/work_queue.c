// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// work_queue.c
// The pthread FIFO worker -- see work_queue.h.

#include "work_queue.h"

// Pop the oldest waiting item (lock held; the queue is not empty).
static wq_item_t *take_waiting(work_queue_t *q) {
    wq_item_t *it = q->head;
    q->head = it->next;
    if (!q->head)
        q->tail = NULL;
    it->next = NULL;
    return it;
}

// The worker: one item at a time, in the order they were pushed.
// With the lock held: file `item` on the done list, after those already there.
static void wq_done_append_locked(work_queue_t *q, wq_item_t *item) {
    item->next = NULL;
    if (q->done_tail)
        q->done_tail->next = item;
    else
        __atomic_store_n(&q->done, item, __ATOMIC_RELEASE); // read lock-free by the owners' pending checks
    q->done_tail = item;
}

static void *wq_main(void *arg) {
    work_queue_t *q = arg;
    for (;;) {
        pthread_mutex_lock(&q->mu);
        while (!q->head)
            pthread_cond_wait(&q->cv, &q->mu);
        wq_item_t *it = take_waiting(q);
        q->active = it;
        pthread_mutex_unlock(&q->mu);

        q->run(q, it);

        pthread_mutex_lock(&q->mu);
        q->active = NULL;
        // Appended, so completions are reported in the order the items ran
        wq_done_append_locked(q, it);
        void (*wake)(void) = q->wake;
        pthread_mutex_unlock(&q->mu);
        if (wake)
            wake();
    }
    return NULL;
}

bool wq_start(work_queue_t *q, size_t stack_bytes) {
    if (q->running)
        return true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_bytes)
        pthread_attr_setstacksize(&attr, stack_bytes);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int rc = pthread_create(&t, &attr, wq_main, q);
    pthread_attr_destroy(&attr);
    q->running = rc == 0;
    return q->running;
}

void wq_push(work_queue_t *q, wq_item_t *item) {
    item->next = NULL;
    pthread_mutex_lock(&q->mu);
    if (q->tail)
        q->tail->next = item;
    else
        q->head = item;
    q->tail = item;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

void wq_lock(work_queue_t *q) {
    pthread_mutex_lock(&q->mu);
}

void wq_unlock(work_queue_t *q) {
    pthread_mutex_unlock(&q->mu);
}

wq_item_t *wq_done_pop(work_queue_t *q) {
    pthread_mutex_lock(&q->mu);
    wq_item_t *it = q->done;
    if (it) {
        __atomic_store_n(&q->done, it->next, __ATOMIC_RELEASE);
        if (!q->done)
            q->done_tail = NULL;
        it->next = NULL;
    }
    pthread_mutex_unlock(&q->mu);
    return it;
}
