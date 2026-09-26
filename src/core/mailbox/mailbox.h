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
//              empty for none.  `deadline_ms` is advisory in Phase 1.
//   EVT_RESULT {id, ok, json_len} + json
//              gs_eval's answer: ok = 1 when it returned 0, else 0 (the
//              JSON then carries {"error": ...}).
//
// Later phases add REQ_SCRIPT / REQ_CANCEL / REQ_MODE_STOP / REQ_ACK_BUF
// and EVT_PROGRESS / EVT_STATE / EVT_NOTIFY / EVT_LOG; the kinds are
// reserved here so the version need not move for each.
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
#define GS_MAILBOX_VERSION 8u

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
#define GS_MBX_REQ_SCRIPT    2u // reserved: Phase 2
#define GS_MBX_REQ_CANCEL    3u // reserved: Phase 2
#define GS_MBX_REQ_MODE_STOP 4u // reserved: Phase 2
#define GS_MBX_REQ_ACK_BUF   5u // reserved: Phase 4
#define GS_MBX_EVT_RESULT    16u
#define GS_MBX_EVT_PROGRESS  17u // reserved: Phase 4
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
// EVT_RESULT payload words.
#define GS_MBX_RESULT_ID       0
#define GS_MBX_RESULT_OK       1
#define GS_MBX_RESULT_JSON_LEN 2
#define GS_MBX_RESULT_WORDS    3
// EVT_STATE / EVT_NOTIFY / EVT_LOG payload words: {json_len} + json.
#define GS_MBX_EVENT_JSON_LEN 0
#define GS_MBX_EVENT_WORDS    1

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

// True when a request is waiting (a cheap peek for the idle wait).
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

// Platform hook: wake whoever waits on a control word (the client parks in
// Atomics.waitAsync on EVT_HEAD and READY).  Weak no-op by default.
void gs_mailbox_notify(volatile uint32_t *word);

#endif // GS_MAILBOX_H
