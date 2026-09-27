// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// io_worker.h -- host file I/O off the emulator thread.
//
// A write whose cost is proportional to the size of a file, not the size
// of the machine, does not belong on the thread that runs the machine.
// The I/O worker is one thread, created at boot, that takes such work
// from a queue: today the checkpoint's publish (write a buffer to a tmp
// path, rename it over the final one), in chunks, yielding between them
// so the guest's own disk reads are not queued behind one long write on
// the filesystem's proxy thread.  Completions are reported back on the
// emulator thread, from the mailbox drain, through the job's callback --
// the emulator thread never waits on the worker except where a caller
// asks to (io_worker_wait_idle, bounded by the write in flight).
//
// Without a worker (a build that did not start one, `--io=sync`), every
// submit returns 0 and the caller does the same work inline through
// io_write_publish: one code path, two threads.

#ifndef GS_IO_WORKER_H
#define GS_IO_WORKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bytes per write call; a knob for measuring (GS_IO_CHUNK_KB at build time).
#ifndef GS_IO_CHUNK_KB
#define GS_IO_CHUNK_KB 1024
#endif

// Runs when a job ends, on the emulator thread.  `ok`, the wall time the
// job took, an error text (NULL when ok) and the caller's context.
typedef void (*io_done_fn)(bool ok, double ms, const char *error, void *ud);

// Starts the worker; false when it could not be created (work then runs
// inline).  Once, at boot.
bool io_worker_start(size_t stack_bytes);
bool io_worker_running(void);

// Publish: unlink tmp_path, write buf[len] to it in chunks, close, rename
// over final_path.  The buffer belongs to the worker until `done` has run.
// Returns the job id, or 0 when there is no worker (nothing was queued).
uint32_t io_submit_publish(const uint8_t *buf, size_t len, const char *tmp_path, const char *final_path,
                           io_done_fn done, void *ud);

// Any work as a job: `work(ud, err, err_cap)` runs on the worker and
// returns 0 or -errno (err: a short description); `done` reports on the
// emulator thread.  0 when there is no worker (the caller works now).
typedef int (*io_work_fn)(void *ud, char *err, size_t err_cap);
uint32_t io_submit_work(io_work_fn work, void *ud, io_done_fn done, void *dud);

// The same publish, here and now, on the calling thread.  0 on success,
// else -errno; `err` (may be NULL) gets a short description.
int io_write_publish(const uint8_t *buf, size_t len, const char *tmp_path, const char *final_path, char *err,
                     size_t err_cap);

// Emulator thread: completions waiting to be reported (cheap), and
// reporting them (runs the callbacks).  Returns how many ran.
bool io_worker_has_work(void);
int io_worker_service(void);

// True while a job is queued, running, or completed but not yet reported.
bool io_worker_busy(void);

// Emulator thread: waits until nothing is in flight, reporting completions
// as they land.  Bounded by the work already queued.
void io_worker_wait_idle(void);

// How to wake the emulator thread when a completion lands (the mailbox
// installs its notify); NULL: nobody parks.
void io_worker_set_waker(void (*wake)(void));

// --- Progress, cancel and hand-offs -----------------------------------------
//
// A job's work reports what it has done, asks whether it was cancelled, and
// can hand a note to the emulator thread; the emulator thread cancels a
// job by id and acknowledges a hand-off.  Every one of these is also
// correct when the work runs inline (no worker): progress then reaches the
// observer at once, on the calling thread, and nothing waits.

// Reports progress from inside the work: `done` of `total` (bytes, files;
// total 0 when unknown).  Coalesced: the observer sees the latest values at
// the emulator thread's next service, never every step.  No-op outside a
// job's work.
// (Weak prototypes: a build that links no io_worker.c -- a unit suite of the
// copy code alone -- sees NULL, and the helpers below then report nothing.)
void io_progress(uint64_t done, uint64_t total) __attribute__((weak));

// True once the job running on this thread was cancelled (io_worker_cancel):
// the work stops at its next chunk, undoes what it can, and returns
// -ECANCELED.  False outside a job's work.
bool io_cancelled(void) __attribute__((weak));

// The null-safe forms every copy loop uses.
static inline void io_report_progress(uint64_t done, uint64_t total) {
    if (io_progress)
        io_progress(done, total);
}
static inline bool io_check_cancelled(void) {
    return io_cancelled && io_cancelled();
}

// Hands a note (a JSON document, at most IO_NOTE_MAX bytes) to the job's
// `note` observer on the emulator thread at its next service.  Notes are
// delivered in order and never dropped; the queue holds a few, and a
// worker that needs the emulator thread to have seen one first waits
// with io_wait_ack.  False when the note did not fit.
#define IO_NOTE_MAX 512
bool io_note(const char *json);

// Waits, on the worker, until the emulator thread acknowledges `tag` (0:
// any tag) for this job (io_worker_ack), the job is cancelled, or `ms` pass: 0,
// -ECANCELED or -ETIMEDOUT.  Inline (no worker) the wait cannot be
// satisfied and returns -ETIMEDOUT at once.
int io_wait_ack(uint32_t tag, unsigned ms);

// Observers a job may carry (emulator thread; either may be NULL).
typedef void (*io_progress_fn)(uint64_t done, uint64_t total, void *ud);
typedef void (*io_note_fn)(const char *json, void *ud);
typedef struct io_job_desc {
    io_work_fn work;
    void *work_ud;
    io_done_fn done;
    void *done_ud;
    io_progress_fn progress;
    io_note_fn note;
    void *observer_ud; // passed to progress and note
} io_job_desc_t;

// Like io_submit_work, with observers.  0 when there is no worker.
uint32_t io_submit_job(const io_job_desc_t *d);

// Emulator thread: cancels the job `id` -- a queued one never runs (its
// done reports "cancelled"), a running one sees io_cancelled().  False
// when no such job is queued or running.
bool io_worker_cancel(uint32_t id);

// Emulator thread: acknowledges `tag` to the job `id` parked in
// io_wait_ack.  False when the job is not running.
bool io_worker_ack(uint32_t id, uint32_t tag);

// Inline observers: when work runs on the calling thread (no worker, or a
// leaf that works now), io_progress and io_note report to these for the
// duration of io_run_inline, which runs `work` with the observers bound
// and returns its result.
int io_run_inline(const io_job_desc_t *d, char *err, size_t err_cap);

// Wall clock in milliseconds (for timing a publish).
double io_now_ms(void);

// Statistics: jobs completed, and how long the last one took.
uint32_t io_worker_jobs_done(void);
double io_worker_last_ms(void);

#endif // GS_IO_WORKER_H
