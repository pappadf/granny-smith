// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mailbox.h
// The mailbox: how clients reach the emulator thread.  One control block
// and two record rings (mailbox_ring.h) in memory both sides can see: a
// REQUEST ring the client writes and the emulator thread drains at frame
// boundaries, and an EVENT ring the emulator thread writes and the client
// reads.  Every request carries an id the client chose; every result
// carries it back, so a reply can never be mistaken for another call's
// answer and many requests can be in flight at once.
//
// This file is platform-independent: the browser page feeds the request
// ring through shared wasm memory (app/web2/src/bus/mailbox.ts is the
// mirror of this header), the headless daemon will feed it from a socket,
// and the unit suite feeds it from the same process.  The platform decides
// where the region lives, who wakes whom (gs_mailbox_notify) and what the
// drain budget is; this file decides what the records mean.
//
// Control block, 32 uint32 words, 64-byte aligned, then the two rings:
//
//   [0]  MAGIC      'GSMB'
//   [1]  VERSION    GS_MAILBOX_VERSION (continues the js_bridge_t count)
//   [2]  REQ_OFF    [3] REQ_SIZE     request ring, client -> core
//   [4]  EVT_OFF    [5] EVT_SIZE     event ring, core -> client
//   [6]  REQ_HEAD   (client writes)  [7] REQ_TAIL (core writes)
//   [8]  EVT_HEAD   (core writes)    [9] EVT_TAIL (client writes)
//   [10] STATUS     DETACHED / ATTACHED / LOST
//   [11] HEARTBEAT  core: bumped once per tick and once per idle drain
//   [12] READY      core: 1 once requests can be served (after shell_init)
//   [13] GPU_AVAILABLE  client -> core, before any machine boots
//   [14..] statistics (see GS_MBX_C_STAT_*)
//
// Record kinds and payloads (little-endian u32 words, then text fields
// each padded to 4; the whole record padded to 8, mailbox_ring.h):
//
//   REQ_EVAL   {id, client, deadline_ms, path_len, args_len} + path + args
//              A gs_eval leaf.  `args` is the JSON arguments document, or
//              empty for none.  `deadline_ms` is advisory.
//   REQ_SCRIPT {id, client, deadline_ms, src_len} + src
//              A script as a job (job.h); the result is the prompt.
//   REQ_CANCEL {id, client, target_id}      cancel the job or I/O job that
//              answers request `target_id` of this client
//   REQ_MODE_STOP {id, client, owner}       stop a mode by owner (0: any)
//   REQ_ACK_BUF {id, client, handle}        the client has consumed a
//              staged buffer (below); the core frees or refills it
//   EVT_RESULT {id, ok, json_len, out_len} + json + output
//              The answer: ok = 1 when the leaf or job succeeded, else 0
//              (the JSON then carries {"error": ...}); `output` is the text
//              the leaf printed while it ran (gs_out.h), when the platform
//              captures it.  A result too large for the event ring arrives
//              as {"$buf": handle, "ptr": p, "len": n}: the JSON lies in a
//              STAGED BUFFER in the core's heap, which the client reads
//              and then releases with REQ_ACK_BUF.
//   EVT_PROGRESS {json_len} + {"id": request, "done": n, "total": n}
//              An I/O job's progress (bytes, files; total 0 when unknown).
//   EVT_STATE / EVT_NOTIFY / EVT_LOG {json_len} + json
//              A core event (gs_event.h); a job's printed output arrives as
//              EVT_LOG {"event":"output","id":request,"client":c,"text":...}
//              records in the order the job produced it.  Annotation
//              records sit among them at the positions they describe:
//              {"event":"value_begin",…} and {"event":"value",…,"json":…}
//              bracket the text of a value the REPL printed, and
//              {"event":"error",…,"file","line","message","lines"} marks a
//              statement error (its text went to stderr).  Every record is
//              at most a quarter of the event ring (gs_mailbox_record_max).
//
// The drain never blocks: a result the event ring has no room for is
// held back and retried at the next drain, and no further requests are
// served until it fits (the client is expected to read events promptly;
// the stall is counted).  Corrupt framing on the request ring marks the
// mailbox LOST and stops the drain for good.

#ifndef GS_MAILBOX_H
#define GS_MAILBOX_H

#include "mailbox_ring.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GS_MAILBOX_MAGIC   0x47534D42u // 'GSMB'
#define GS_MAILBOX_VERSION 9u

#define GS_MBX_CTRL_WORDS 32
#define GS_MBX_ALIGN      64u

// Ring sizes.  A test may override them (-DGS_MBX_REQ_BYTES=...) to force
// wraps; the client reads the sizes from the control block.
#ifndef GS_MBX_REQ_BYTES
#define GS_MBX_REQ_BYTES (256u << 10)
#endif
#ifndef GS_MBX_EVT_BYTES
#define GS_MBX_EVT_BYTES (1024u << 10)
#endif
// The largest answer gs_eval may produce (the old bridge's output slot).
#define GS_MBX_RESULT_MAX (256u << 10)
// The largest request path and arguments document accepted.
#define GS_MBX_PATH_MAX 1023u
#define GS_MBX_ARGS_MAX (128u << 10)

// Control-block word indices.
#define GS_MBX_C_MAGIC         0
#define GS_MBX_C_VERSION       1
#define GS_MBX_C_REQ_OFF       2
#define GS_MBX_C_REQ_SIZE      3
#define GS_MBX_C_EVT_OFF       4
#define GS_MBX_C_EVT_SIZE      5
#define GS_MBX_C_REQ_HEAD      6
#define GS_MBX_C_REQ_TAIL      7
#define GS_MBX_C_EVT_HEAD      8
#define GS_MBX_C_EVT_TAIL      9
#define GS_MBX_C_STATUS        10
#define GS_MBX_C_HEARTBEAT     11
#define GS_MBX_C_READY         12
#define GS_MBX_C_GPU_AVAILABLE 13
#define GS_MBX_C_STAT_REQUESTS 14 // requests served
#define GS_MBX_C_STAT_EVENTS   15 // events written
#define GS_MBX_C_STAT_STALLS   16 // drains that found no room for a result
#define GS_MBX_C_STAT_DRAIN_US 17 // the longest drain so far, microseconds
#define GS_MBX_C_STAT_BAD      18 // requests refused (unknown kind, bad lengths)
#define GS_MBX_C_STAT_DROPPED  19 // core events dropped for lack of event-ring room

#define GS_MBX_STATUS_DETACHED 0u
#define GS_MBX_STATUS_ATTACHED 1u
#define GS_MBX_STATUS_LOST     2u

// Record kinds.  0 is the PAD (mailbox_ring.h).
#define GS_MBX_REQ_EVAL      1u
#define GS_MBX_REQ_SCRIPT    2u // run a script as a job; the result comes when it ends
#define GS_MBX_REQ_CANCEL    3u // cancel a job of this client
#define GS_MBX_REQ_MODE_STOP 4u // stop a mode by owner (0: any)
#define GS_MBX_REQ_ACK_BUF   5u // a staged buffer was consumed: {id, client, handle}
#define GS_MBX_EVT_RESULT    16u
#define GS_MBX_EVT_PROGRESS  17u // an I/O job's progress: {json_len} + json
#define GS_MBX_EVT_STATE     18u // a core event (gs_event.h): run state
#define GS_MBX_EVT_NOTIFY    19u // a core event: something the UI shows
#define GS_MBX_EVT_LOG       20u // a core event: a log line

// REQ_EVAL payload words.
#define GS_MBX_EVAL_ID       0
#define GS_MBX_EVAL_CLIENT   1
#define GS_MBX_EVAL_DEADLINE 2
#define GS_MBX_EVAL_PATH_LEN 3
#define GS_MBX_EVAL_ARGS_LEN 4
#define GS_MBX_EVAL_WORDS    5
// REQ_SCRIPT payload words: {id, client, deadline_ms, src_len} + src.
#define GS_MBX_SCRIPT_ID       0
#define GS_MBX_SCRIPT_CLIENT   1
#define GS_MBX_SCRIPT_DEADLINE 2
#define GS_MBX_SCRIPT_SRC_LEN  3
#define GS_MBX_SCRIPT_WORDS    4
#define GS_MBX_SCRIPT_MAX      (256u << 10)
// REQ_CANCEL payload words: {id, client, target_id}.
// REQ_MODE_STOP payload words: {id, client, owner}.
#define GS_MBX_CTL_ID     0
#define GS_MBX_CTL_CLIENT 1
#define GS_MBX_CTL_ARG    2
#define GS_MBX_CTL_WORDS  3
// EVT_RESULT payload words: {id, ok, json_len, out_len} + json + output
// (each text padded to 4).
#define GS_MBX_RESULT_ID       0
#define GS_MBX_RESULT_OK       1
#define GS_MBX_RESULT_JSON_LEN 2
#define GS_MBX_RESULT_OUT_LEN  3
#define GS_MBX_RESULT_WORDS    4
// The most output one answer carries; beyond it the rest is dropped and
// the text ends in "...".
#define GS_MBX_OUTPUT_MAX (64u << 10)
// Staged buffers: results spilled out of the ring, and I/O hand-offs.
#define GS_MBX_STAGED_MAX 8
#define GS_MBX_SPILL_MAX  (64u << 20) // the largest result that is spilled rather than refused
// EVT_STATE / EVT_NOTIFY / EVT_LOG payload words: {json_len} + json.
#define GS_MBX_EVENT_JSON_LEN 0
#define GS_MBX_EVENT_WORDS    1

#define GS_MBX_DEFER_MAX 16 // leaves with an answer still to come

// The leaf executor: gs_eval's signature.  Injected so the unit suite can
// drive the mailbox without the object model.
typedef int (*gs_mailbox_eval_fn)(const char *path, const char *args_json, char *out, size_t out_size);

typedef struct gs_mailbox {
    volatile uint32_t *ctrl;
    mbx_ring_t req; // this side reads
    mbx_ring_t evt; // this side writes
    gs_mailbox_eval_fn eval;
    // The answer being built or held back: gs_eval's output and the id it
    // belongs to.  `held` is set when the event ring had no room for it.
    char *out;
    uint32_t out_len;
    uint32_t out_id;
    int out_ok;
    bool held;
    // Scratch for a request's NUL-terminated path and arguments.
    char path[GS_MBX_PATH_MAX + 1];
    char *args;
    uint32_t heartbeat;
    uint32_t client; // the client whose request is being served, 0 between requests
    // Deferred results: a leaf that finishes later (an I/O job) takes its
    // request off the answer path with gs_result_defer and answers it
    // through gs_result_complete.
    bool serving; // inside serve_eval
    bool deferred; // the leaf being served deferred its answer
    struct {
        uint32_t token;
        uint32_t req_id;
        uint32_t io_job; // the I/O job answering it (0: none), for REQ_CANCEL
    } defers[GS_MBX_DEFER_MAX];
    int n_defers;
    uint32_t defer_seq;
    // Output the leaf being served printed (gs_out.h), when captured.
    bool capture_output;
    char *outbuf;
    uint32_t outbuf_len;
    bool outbuf_cut;
    uint32_t held_out_len; // the output that goes with a held answer
} gs_mailbox_t;

// Bytes the whole region needs (alignment slack included) for the given
// ring sizes.
size_t gs_mailbox_region_bytes(uint32_t req_bytes, uint32_t evt_bytes);

// Lays the control block and rings out in `region` (which must hold
// gs_mailbox_region_bytes) and binds `m` to them.  Allocates the output
// and argument scratch.  Returns the 64-byte-aligned control block
// address, or NULL on allocation failure.  READY stays 0: the platform
// sets it (gs_mailbox_set_ready) once leaves can be served.
volatile uint32_t *gs_mailbox_init(gs_mailbox_t *m, void *region, uint32_t req_bytes, uint32_t evt_bytes,
                                   gs_mailbox_eval_fn eval);

// Frees the scratch (not the region).
void gs_mailbox_free(gs_mailbox_t *m);

// Sets READY and wakes a client parked on it.
void gs_mailbox_set_ready(gs_mailbox_t *m);

// Bumps HEARTBEAT: once per tick, and once per idle drain on a stopped
// machine, so a client can tell "slow" from "dead" (a heartbeat that stops
// while requests are pending).
void gs_mailbox_heartbeat(gs_mailbox_t *m);

// Serves every request on the ring, in order, until it is empty, a result
// cannot be written (held back for the next drain), the framing breaks
// (LOST), or `budget_us` host microseconds have passed -- a leaf that is
// running when the budget expires still completes; the budget bounds
// bursts of many small requests, not one leaf.  `now_us` is the platform's
// clock (NULL: no budget).  Returns the number of results written; the
// platform wakes the client on EVT_HEAD when that is non-zero.
int gs_mailbox_drain(gs_mailbox_t *m, double budget_us, double (*now_us)(void));

// True when a request, a job's call or a job's result is waiting (a cheap
// peek for the idle wait).
bool gs_mailbox_has_requests(const gs_mailbox_t *m);

// Writes one core event (`kind` is GS_MBX_EVT_STATE / NOTIFY / LOG, the
// payload {json_len} + json) and publishes it at once, waking the client.
// Emulator thread only, from anywhere in a leaf or the tick: no record is
// ever half-written across a call, so publishing mid-drain is safe.  False
// when the event ring has no room: the event is dropped and counted
// (STAT_DROPPED) -- an event never blocks the emulator thread.
bool gs_mailbox_emit(gs_mailbox_t *m, uint32_t kind, const char *json);

// The client whose request is being served, 0 outside a drain.
uint32_t gs_mailbox_current_client(const gs_mailbox_t *m);

// Writes one EVT_LOG output record (a job's printed text, job.c) without
// publishing; false when the ring has no room -- output is never dropped,
// the job keeps it for the next drain.
bool gs_mailbox_emit_output(gs_mailbox_t *m, const char *json);

// The largest record (header + JSON) any producer should write: a quarter of
// the event ring, so a record always fits -- mbx_reserve can never place one
// longer than the ring, and with wrap padding may fail for good on one
// longer than half of it.  Set once by gs_mailbox_init; readable from any
// thread.
void gs_mailbox_set_record_max(size_t bytes);
size_t gs_mailbox_record_max(void);

// Writes one EVT_RESULT (not published: the drain publishes).  False when
// the event ring has no room.  For the job layer, whose results arrive
// when a job ends rather than when a request is served.  `output` (may be
// NULL) is the captured text that goes with the answer.
bool gs_mailbox_write_result(gs_mailbox_t *m, uint32_t id, bool ok, const char *json, uint32_t len, const char *output,
                             uint32_t out_len);

// Writes one EVT_PROGRESS for request `id` and publishes it (dropped and
// counted when the ring has no room, like an event).
bool gs_mailbox_write_progress(gs_mailbox_t *m, uint32_t id, uint64_t done, uint64_t total);

// Whether answers carry the output the leaf printed (gs_out.h routes a
// served leaf's stdout into the answer when this is set; the page wants
// that, the headless driver prints as it goes).
void gs_mailbox_set_capture_output(gs_mailbox_t *m, bool on);
// gs_out.c: appends to the output of the request being served; false when
// nothing is being served that captures.
bool gs_mailbox_output_append(const char *text, size_t len);
// The client whose request the process's mailbox is serving (0: none).
uint32_t gs_mailbox_serving_client(void);

// --- Staged buffers -----------------------------------------------------------
// A region of the core's heap described to the client by {handle, ptr,
// len} inside an event or a result; the client reads (or writes) it
// through the shared memory and releases it with REQ_ACK_BUF.  A buffer
// bound to an I/O job (`io_job` non-zero) is the job's own: the ack is
// forwarded to it (io_worker_ack) and the job refills; an unbound one is
// freed on ack.  Emulator thread only.  Returns the handle, 0 when the
// table is full.
uint32_t gs_staged_publish(void *ptr, size_t len, uint32_t io_job);
// Releases a handle the way an ack would (a job that ends unlinks its own).
void gs_staged_release(uint32_t handle);
bool gs_staged_lookup(uint32_t handle, void **ptr, size_t *len);

// --- An in-process client ---------------------------------------------------
// The other side of the same mailbox, in C: what the headless driver's
// stdin, script file and daemon socket are.  Posts records on the request
// ring and reads results off the event ring; the caller drives the
// emulator thread's loop (frame, drain) in between.  Single client per
// struct, same thread as the emulator (this is not a second producer: the
// page and this never coexist in one process).
typedef struct gs_mailbox_client {
    gs_mailbox_t *m;
    mbx_ring_t req; // this side writes
    mbx_ring_t evt; // this side reads
    uint32_t next_id;
} gs_mailbox_client_t;

void gs_mailbox_client_init(gs_mailbox_client_t *c, gs_mailbox_t *m);
// Posts a REQ_SCRIPT for `client`; returns its id, 0 when the ring is full
// or the source too large.
uint32_t gs_mailbox_client_script(gs_mailbox_client_t *c, uint32_t client, const char *src, size_t len);
// Posts a REQ_CANCEL / REQ_MODE_STOP; returns the id, 0 when no room.
uint32_t gs_mailbox_client_cancel(gs_mailbox_client_t *c, uint32_t client, uint32_t target_id);
uint32_t gs_mailbox_client_mode_stop(gs_mailbox_client_t *c, uint32_t client, uint32_t owner);
// Takes the next event off the ring: its kind (0: none, PADs skipped),
// the payload copied into `buf` (at most `cap` bytes; *len the real size).
uint32_t gs_mailbox_client_take(gs_mailbox_client_t *c, uint8_t *buf, size_t cap, uint32_t *len);

// --- Deferred results --------------------------------------------------------
// A leaf whose work goes to the I/O worker answers later: it calls
// gs_result_defer() while it is being served, gets a token, returns any
// value (dropped), and the completion calls gs_result_complete*.  The
// token is 0 when nothing is being served that can wait (the leaf then
// does its work now).  Works for a page request (the EVT_RESULT is written
// at completion) and for a job's call through the seam (the job is held,
// and a failure becomes the call's error).  Emulator thread only.
uint32_t gs_result_defer(void);
void gs_result_complete(uint32_t token, bool ok, const char *json);
void gs_result_complete_ok(uint32_t token);
void gs_result_complete_error(uint32_t token, const char *message);
// Names the I/O job answering the deferral, so a REQ_CANCEL of the request
// (or of the script whose call it is) cancels the job.
void gs_result_bind_io(uint32_t token, uint32_t io_job);
// Reports an I/O job's progress to the client that asked: an EVT_PROGRESS
// for the request (for a script's call: for the script's request).
void gs_result_progress(uint32_t token, uint64_t done, uint64_t total);
// The request id a token answers (0: unknown), for events that name it.
uint32_t gs_result_request_id(uint32_t token);

// Platform hook: wake whoever waits on a control word (the client parks in
// Atomics.waitAsync on EVT_HEAD and READY).  Weak no-op by default.
void gs_mailbox_notify(volatile uint32_t *word);

#endif // GS_MAILBOX_H
