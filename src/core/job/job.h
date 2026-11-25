// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// job.h -- jobs: scripts, run as coroutines on the emulator thread.
//
// The emulator thread owns all guest state and runs nothing of unbounded
// length: it ticks frames and drains the mailbox.  A script is unbounded,
// so a script is a JOB: queued FIFO, one active at a time, and run as a
// TASK -- a coroutine of the script engine (gs_lua.c) -- that the drain
// steps (job_layer_service).  A step runs until the script ends or
// suspends: after a tree call that started a mode the client waits for
// (`scheduler.run(N)`), while a leaf's deferred answer is outstanding (an
// I/O job), or when its time slice is spent.  The drain resumes it once
// the wait is over, so `scheduler.run(N)` inside a script means "run N" on
// every platform, and a tree call is a plain C call: there is no second
// thread and nothing to marshal (docs/internals/core/job/job.md).
//
// A tree call that cannot suspend (a breakpoint condition, a chunk run
// synchronously from a request, a `require` at load time) waits by running
// frames where the platform registered a frame runner (headless), and does
// not wait where it did not (the browser).

#ifndef GS_JOB_H
#define GS_JOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct value;

struct mailbox;
typedef struct job job_t;

// Records the calling thread as the emulator thread: the one latch, once, at
// core_init (docs/internals/core/job/job.md).  A debug build asserts that no
// other thread latched first.
void job_layer_init(void);

// True on the emulator thread (or before job_layer_init: single-threaded).
bool job_on_emulator_thread(void);

// The thread-affinity guard for the gateways into emulator state
// (object_eval, shell_complete, llap_receive, afp_handle_command): asserts
// the caller is the emulator thread, latching it when nothing has yet.
// Compiled out unless GS_DEBUG; `where` names the gateway and is not
// evaluated in a release build.
#ifdef GS_DEBUG
void job_emulator_thread_check(const char *where);
#define EMU_THREAD_CHECK(where) job_emulator_thread_check(where)
#else
#define EMU_THREAD_CHECK(where) ((void)0)
#endif

// Queues a script.  `req_id` is the mailbox request it answers (the result
// is written by job_layer_service when the job ends).  False when out of
// memory.
bool job_submit_script(uint32_t client, uint32_t req_id, const char *src, size_t len);

// Cancels the job (client, req_id) / every job of a client: the flag is
// set, the job's mode and deferred I/O are stopped, and the task unwinds
// when it next runs.  Returns the number of jobs found.
int job_cancel(uint32_t client, uint32_t req_id);
int job_cancel_client(uint32_t client);

// The job whose task is running right now, NULL outside a step.
job_t *job_current(void);
uint32_t job_current_client(void);
// The engine's cancel check (its instruction hook): the running job was
// cancelled.
bool job_current_cancelled(void);

// The word the emulator thread parks on when idle (mailbox.c sets it).
void job_layer_set_wake_word(volatile uint32_t *word);

// Emulator thread: is there a job to start or resume, output to deliver, or
// a finished job's result to write?  Cheap.
bool job_layer_has_work(void);
// Emulator thread, from the drain: deliver output, start or resume the
// active job's task for one step, write the results of finished jobs.
// Returns the number of results written.
int job_layer_service(struct mailbox *m);

// True while a job's task is running (the leaf running now was called by a
// script).
bool job_serving_call(void);

// --- A task's tree calls (the engine's side) -----------------------------------
// Around each tree call a task makes: begin notes the mode and whether the
// task can suspend at this point (`can_suspend`: the engine can yield
// here); end says whether it must suspend now -- a mode the client waits
// for began, or the leaf deferred its answer.  After it resumes (or at
// once, when end says no) the engine takes a deferred failure or answer
// with job_call_take_failure / job_call_take_answer.  A call that cannot
// suspend never defers, and waits for its mode with the frame runner
// (job_set_frame_runner) when there is one.  Outside a job's step, end
// does that wait for the client being served.
typedef struct {
    uint32_t mode; // the mode id before the call
    bool prev_can_suspend; // the enclosing call's setting (calls nest)
    bool in_job;
} job_call_mark_t;
job_call_mark_t job_task_call_begin(bool can_suspend);
bool job_task_call_end(job_call_mark_t mark);

// The platform's frame runner: one frame (or a short sleep when the machine
// is stopped).  Headless registers one; a tree call that cannot suspend
// runs it until the mode it started ends.
void job_set_frame_runner(void (*run_frame)(void));

// A leaf called by a job's task may answer later (mailbox_result_defer):
// the task stays suspended until job_call_complete(token) -- tokens have the
// top bit set -- and a failure becomes the call's error, which the engine
// takes with job_call_take_failure when the task resumes.
uint32_t job_call_defer(void); // 0 when no call is being served
void job_call_complete(uint32_t token, bool ok, const char *json);
bool job_call_take_failure(char *err, size_t cap);
// A deferred call's success answer as a value (an I/O leaf's), kept whole
// for the task -- its kind and presentation intact, which the JSON of a
// request's answer would not keep -- and taken with job_call_take_answer
// when the task resumes, in place of the provisional value the leaf
// returned at once.  False when `token` is not a job's call (a request's
// deferral, answered with mailbox_result_complete).
struct value;
bool job_call_complete_value(uint32_t token, const struct value *v);
bool job_call_take_answer(struct value *out); // false: none (out untouched)
// The I/O job answering the deferred call (a cancel of the script cancels
// it), and the request id of the script the call belongs to.
void job_call_bind_io(uint32_t token, uint32_t io_job);
uint32_t job_call_request_id(uint32_t token);

// --- Output ------------------------------------------------------------------
// out.c: appends text to the job whose task is running.  False when there
// is none (the text goes elsewhere).  The
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

// --- Glue the engine-neutral job.c needs from the rest of the core (a unit
// suite stubs these) ----------------------------------------------------------
// A task for one job's source (a terminal line or a chunk): NULL when it
// could not be made -- the source does not compile; the error has been
// reported to the job.  Called during the job's first step.
typedef struct job_task job_task_t;
job_task_t *job_glue_task_new(const char *src);
// Runs the task until it ends (true; *rc 0 ok, -1 failed) or suspends
// (false).
bool job_glue_task_step(job_task_t *task, int *rc);
void job_glue_task_free(job_task_t *task);
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
