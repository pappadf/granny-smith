// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// job.h -- jobs: work the emulator thread does not do itself.
//
// The emulator thread owns all guest state and runs nothing of unbounded
// length: it ticks frames and drains the mailbox.  A script is unbounded,
// so a script is a JOB, executed on the one job thread, FIFO.  That thread
// touches nothing but the interpreter's own memory; every read or write of
// the object tree is handed to the emulator thread through
// job_on_emulator(), the SEAM, and the job waits for the result exactly as
// the page waits for its request -- served at the next frame boundary, in
// the same drain.  A leaf that starts a mode (scheduler.run) makes the job
// wait until that mode ends, so `scheduler.run N` inside a script means
// "run N" on every platform.
//
// A build or moment without a job thread (headless today, the unit
// suites, the inline `shell.eval` leaf) runs the interpreter on the
// emulator thread: the seam is then a direct call and a submitted script
// runs inline.  Nothing in the interpreter knows which.
//
// The tables the interpreter and the emulator thread both read -- scopes,
// aliases, functions -- sit behind job_tables_lock(), taken for one
// operation at a time and never across a request.

#ifndef GS_JOB_H
#define GS_JOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct value;

struct gs_mailbox;
typedef struct gs_job gs_job_t;

// Records the calling thread as the emulator thread.  Once, at core_init.
void job_layer_init(void);

// True on the emulator thread (or before job_layer_init: single-threaded).
bool job_on_emulator_thread(void);

// Starts the job thread with the given stack.  Platforms with one call it
// once at boot; false when the thread could not be created (scripts then
// run inline).
bool job_thread_start(size_t stack_bytes);
bool job_thread_running(void);

// THE SEAM.  Runs fn(ud) on the emulator thread and returns when it has
// run: directly when already there, else posted and waited for.  The
// emulator thread serves it from gs_mailbox_drain with the job's client
// as the current client, and holds the job while a mode that call started
// is running.
void job_on_emulator(void (*fn)(void *ud), void *ud);

// Queues a script for the job thread.  `req_id` is the mailbox request it
// answers (the result is written by job_layer_service when the job ends).
// False when there is no job thread: the caller runs the script inline.
bool job_submit_script(uint32_t client, uint32_t req_id, const char *src, size_t len);

// Cancels the job (client, req_id) / every job of a client: the flag is
// set, the job's modes are stopped, and the interpreter unwinds at its
// next statement.  Returns the number of jobs found.
int job_cancel(uint32_t client, uint32_t req_id);
int job_cancel_client(uint32_t client);

// The job the calling thread is running, NULL on the emulator thread.
gs_job_t *job_current(void);
uint32_t job_current_client(void);
// The interpreter's cancel check (every statement).
bool job_current_cancelled(void);

// The word the emulator thread parks on when idle (mailbox.c sets it): a
// job wakes it through gs_mailbox_notify on that word.
void job_layer_set_wake_word(volatile uint32_t *word);

// Emulator thread: is there a call or a finished job to serve?  Cheap.
bool job_layer_has_work(void);
// Emulator thread, from the drain: serve the pending call, release a
// call whose mode has ended, write the results of finished jobs.
// Returns the number of results written.
int job_layer_service(struct gs_mailbox *m);

// The interpreter-table lock (recursive).
void job_tables_lock(void);
void job_tables_unlock(void);

// True while the emulator thread is serving a job's call through the seam
// (the leaf running now was called by a script).
bool job_serving_call(void);

// A leaf served for a job's call may answer later (gs_result_defer): the
// call stays held until job_call_complete(token) -- tokens have the top
// bit set -- and a failure becomes the call's error, which the job thread
// picks up with job_call_take_failure right after the seam returns.
uint32_t job_call_defer(void); // 0 when no call is being served
void job_call_complete(uint32_t token, bool ok, const char *json);
bool job_call_take_failure(char *err, size_t cap);
// The I/O job answering the deferred call (a cancel of the script cancels
// it), and the request id of the script the call belongs to.
void job_call_bind_io(uint32_t token, uint32_t io_job);
uint32_t job_call_request_id(uint32_t token);

// --- Output ------------------------------------------------------------------
// out.c: appends text to the job that owns the calling context -- the
// job running on this thread, or the job whose call the emulator thread is
// serving.  False when there is none (the text goes elsewhere).  The
// buffered text is delivered by job_layer_service as EVT_LOG
// {"event":"output","id":req,"client":c,"text":...} records, in order,
// before the job's result.
bool job_output_append(const char *text, size_t len);

// Places an annotation record in the calling job's record stream at the
// current output position (same context test as job_output_append): after
// the text printed so far, before whatever is printed next.  Written as
// {"event":<kind>,"id":req,"client":c,<fields>}, the entries of the VK_MAP
// `fields` (plain data; tagged JSON) following the header; NULL for none.
// When the full record would exceed the ring's record bound the `reduced`
// map (optional; callers keep it small) is used instead, and
// `used_reduced` (optional) says it was.  The job is resolved once, so the
// ids and the stream are one job's.  False outside a job, past the 1 MiB
// output cut, or when neither form fits.
bool job_annotate(const char *kind, const struct value *fields, const struct value *reduced, bool *used_reduced);

// --- Inline mode --------------------------------------------------------------
// Without a job thread a script runs on the emulator thread inside the
// drain (mailbox.c serve_script), and a leaf that starts a mode cannot be
// waited for by holding a call.  Inline mode supplies the wait: after a
// tree call on the emulator thread that opened a mode the client waits
// for, `run_frame` is called until the mode ends.  `--jobs=inline` on
// headless sets it; the seam's callers (object.c) consult it.
void job_inline_enable(void (*run_frame)(void));
bool job_inline_enabled(void);
// Emulator thread, around a tree call: the mode id before, and the wait
// after (a no-op when nothing changed or the client does not wait).
void job_inline_after_call(uint32_t mode_before);

// --- Between seam.c and job.c ---------------------------------------------
typedef void (*job_post_fn)(void (*fn)(void *ud), void *ud);
void job_seam_set_poster(job_post_fn post);
void job_seam_set_current(gs_job_t *job, uint32_t client, const bool *cancel);
void job_seam_note_failure(const char *error);

// --- Glue the platform-independent job.c needs from the rest of the core
// (a unit suite stubs these) --------------------------------------------
// Runs one script source to completion on the calling thread: 0 ok, -1
// failed.  `interactive` prints REPL results.
int job_glue_run_source(const char *src, bool interactive);
// Whether a mode the job must wait for is running: owned by `client`, and
// either bounded (`scheduler.run N` returns when N has run) or unbounded
// on a client whose scripts wait for the machine to stop (below).
bool job_glue_mode_waits(uint32_t client);
// Whether a bare `scheduler.run` holds this client's script until the
// machine stops (weak; true by default -- the script suites' semantics;
// the browser's terminal says false: the line returns, the machine runs).
bool job_glue_unbounded_waits(uint32_t client);
// The current mode id (0: none yet).
uint32_t job_glue_mode_id(void);
// Stops every mode owned by `client`; returns whether one was.
bool job_glue_stop_modes(uint32_t client);
// Stops the mode `mode_id` if it is the one running and `client` owns it
// (a cancelled job takes down the run it started, not its client's others).
bool job_glue_stop_mode(uint32_t client, uint32_t mode_id);

#endif // GS_JOB_H
