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

// Wall clock in milliseconds (for timing a publish).
double io_now_ms(void);

// Statistics: jobs completed, and how long the last one took.
uint32_t io_worker_jobs_done(void);
double io_worker_last_ms(void);

#endif // GS_IO_WORKER_H
