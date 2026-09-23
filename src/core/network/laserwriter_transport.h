// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// laserwriter_transport.h
// How the LaserWriter bridge (laserwriter_job.c) reaches the PostScript
// interpreter: four requests out, four results back through callbacks,
// always asynchronously.  Two implementations, one linked per build:
//   laserwriter_transport_direct.c (headless) calls EfterScript's platen
//     library and defers every callback through the scheduler;
//   laserwriter_transport_ring.c (wasm) writes records into a shared-memory
//     ring drained by a Web Worker (laserwriter_ring_protocol.h) and
//     dispatches its answers from laserwriter_transport_poll().
// The bridge keeps ONE request outstanding per job (open, then feed or
// finish, one at a time); abandon is fire-and-forget.  Results for a job
// id the bridge no longer holds are dropped.

#ifndef LASERWRITER_TRANSPORT_H
#define LASERWRITER_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Type Definitions ===

// One statusdict identity entry: the key and the value as PostScript
// literal text (the shape of platen_entry, without the library header).
typedef struct {
    const char *key;
    const char *value;
} laserwriter_identity_t;

// The platen_config fields the bridge sets, transport-neutral.  Valid for
// the duration of the open call only; a transport copies what it keeps.
typedef struct {
    const laserwriter_identity_t *identity; // statusdict entries seeded before the prelude
    size_t identity_len;
    const uint8_t *prelude; // a program run once before the job
    size_t prelude_len;
    int32_t server_password; // what exitserver expects
    bool compress; // compress page streams
    bool embed_all_fonts; // embed the resident faces
    uint64_t step_budget; // objects the job may execute; 0 = unlimited
} laserwriter_job_config_t;

// A feed's outcome (platen_job_feed's result, named).
typedef enum {
    LASERWRITER_FEED_WAITING = 0, // the bytes ran; the job waits for more
    LASERWRITER_FEED_ENDED, // the job ended before its data did; finish reports why
    LASERWRITER_FEED_FAILED // a failure code: the job is unusable
} laserwriter_feed_status_t;

// A finished job's outcome (platen_job_finish's, named).
typedef enum {
    LASERWRITER_OUTCOME_OK = 0, // the program ran to the end of its data
    LASERWRITER_OUTCOME_ERROR, // an uncaught error: error_name and offending set
    LASERWRITER_OUTCOME_BUDGET, // the execution budget was spent
    LASERWRITER_OUTCOME_FAILED // the document could not be closed; error_name carries the message
} laserwriter_outcome_t;

// What FINISHED carries.  Pointers are valid for the callback only.
typedef struct {
    laserwriter_outcome_t outcome;
    uint32_t pages; // pages in the finished document
    const char *error_name; // "" unless ERROR (or FAILED: the failure message)
    const char *offending; // "" unless ERROR
    const uint8_t *reply; // standard output produced by the completion
    size_t reply_len;
    const uint8_t *errors; // standard error produced by the completion
    size_t errors_len;
    const uint8_t *pdf; // the document: the direct transport only (NULL from the ring:
    size_t pdf_len; //   the worker posts it to the main thread)
} laserwriter_finish_result_t;

// Results, delivered from the scheduler (direct) or from
// laserwriter_transport_poll() (ring) — never from inside a request call.
typedef struct {
    void (*on_opened)(uint32_t job_id, void *ctx);
    void (*on_open_failed)(uint32_t job_id, const char *error, void *ctx);
    void (*on_fed)(uint32_t job_id, uint32_t sequence, laserwriter_feed_status_t status, uint32_t pages,
                   const uint8_t *reply, size_t reply_len, const uint8_t *errors, size_t errors_len, bool truncated,
                   void *ctx);
    void (*on_finished)(uint32_t job_id, const laserwriter_finish_result_t *result, void *ctx);
} laserwriter_transport_callbacks_t;

// === Operations ===

// Installs the result callbacks (once, at bridge init).
void laserwriter_transport_set_callbacks(const laserwriter_transport_callbacks_t *callbacks, void *ctx);

// Register the transport's timers with the stack's scheduler.  Called from
// laserwriter_job_init each time the stack comes up (atalk_timer_t).
void laserwriter_transport_init(void);

// Starts job `job_id` with `cfg`.  Returns false when the request could
// not be issued (transport out of room or a request already outstanding);
// otherwise on_opened / on_open_failed follows.
bool laserwriter_transport_open(uint32_t job_id, const laserwriter_job_config_t *cfg);

// Feeds `len` bytes (at most one PAP flow quantum) answered by SendData
// `sequence`; on_fed follows with the same sequence.  False when not
// issued.
bool laserwriter_transport_feed(uint32_t job_id, uint32_t sequence, const uint8_t *bytes, size_t len);

// End of data; on_finished follows.  `title` is the job name the bridge
// scanned ("" when none): the ring transport carries it to the worker,
// which names the browser download from it; the direct transport ignores
// it (the document reaches the sink with the bridge's title).  False when
// not issued.
bool laserwriter_transport_finish(uint32_t job_id, const char *title);

// Frees the job without finishing (the connection went away).  No
// callback follows; a result already in flight for the job is dropped.
void laserwriter_transport_abandon(uint32_t job_id);

// Drains pending results and dispatches their callbacks.  The bridge
// calls it from its guest-time tick while a request is outstanding; a
// platform may also call it from its frame loop.  A no-op for the direct
// transport when a scheduler drives it.
void laserwriter_transport_poll(void);

// A short name for logs and the object model: "direct" or "ring".
const char *laserwriter_transport_name(void);

// === Ring transport platform hooks ===
// Weak no-ops in laserwriter_transport_ring.c; the wasm platform overrides
// them (em_main.c): the first asks the page to attach the interpreter
// worker to the control block at `ctrl_addr`, the second wakes a worker
// parked on a control word (Atomics.notify on the address).

void laserwriter_ring_attach_requested(uintptr_t ctrl_addr);
void laserwriter_ring_notify(volatile uint32_t *addr);

#endif // LASERWRITER_TRANSPORT_H
