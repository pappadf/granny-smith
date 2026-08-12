// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_adsp.h
// AppleTalk Data Stream Protocol (ADSP): reliable, full-duplex byte streams
// between two sockets, carried in DDP packets of type 7.
//
// Coded from Inside AppleTalk, 2nd ed., chapter 12 — the complete wire
// specification (packet format 12-12, control packets 12-14, attention
// messages 12-19, connection opening 12-22, closing 12-38).  The in-house
// reference distilled from it is docs/core/network/appletalk.md §III.3;
// implementation code should cite that section.
//
// The engine is instance-based and transport-agnostic: it never touches DDP
// or the scheduler itself, it calls out through `adsp_config_t`.  That keeps
// it unit-testable with two endpoints wired back to back over a loopback
// (tests/unit/suites/adsp/) while the production instance in this file's
// .c hangs off the real stack.
//
// Object-model surface: `appletalk.adsp.connections` / `appletalk.adsp.stats`.

#ifndef APPLETALK_ADSP_H
#define APPLETALK_ADSP_H

#include "appletalk_internal.h"

#include <stdbool.h>
#include <stdint.h>

// === Forward declarations ===
struct object;
typedef struct scheduler scheduler_t;

typedef struct adsp_stack adsp_stack_t;
typedef struct adsp_conn adsp_conn_t;

// === Wire constants (Inside AppleTalk 12-12 ff.) ============================

#define ADSP_HEADER_SIZE      13 // src ConnID, first/next seq, window, descriptor
#define ADSP_OPEN_PARAMS_SIZE 8 // version, dest ConnID, PktAttnRecvSeq
#define ADSP_MAX_DATA         572 // ADSP data bytes per packet (12-12)
#define ADSP_MAX_ATTN_DATA    570 // attention data after the 2-byte code (12-19)
#define ADSP_VERSION          0x0100 // the version this chapter documents (12-27)

// ADSP descriptor bits (12-12).  The low nibble is the control code.
#define ADSP_DESC_CONTROL   0x80
#define ADSP_DESC_ACK_REQ   0x40
#define ADSP_DESC_EOM       0x20
#define ADSP_DESC_ATTENTION 0x10
#define ADSP_DESC_CODE_MASK 0x0F

// ADSP control codes (12-14).  Values 9..15 are reserved and rejected.
#define ADSP_CTL_PROBE_ACK     0
#define ADSP_CTL_OPEN_REQ      1
#define ADSP_CTL_OPEN_ACK      2
#define ADSP_CTL_OPEN_REQ_ACK  3
#define ADSP_CTL_OPEN_DENY     4
#define ADSP_CTL_CLOSE         5
#define ADSP_CTL_FWD_RESET     6
#define ADSP_CTL_FWD_RESET_ACK 7
#define ADSP_CTL_RETRANSMIT    8

// === Engine tunables ========================================================
//
// Every interval is emulated time; the engine has no notion of wall clock, so
// a script that runs the same instruction budget sees the same wire trace.

#define ADSP_MAX_CONNECTIONS 8 // connection ends the engine can hold at once
#define ADSP_SEND_QUEUE      8192 // unacknowledged bytes we are willing to hold
#define ADSP_RECV_WINDOW     4096 // bytes we advertise in PktRecvWdw

#define ADSP_PROBE_INTERVAL_NS 30000000000ull // connection timer (12-5)
#define ADSP_PROBE_LIMIT       4 // fourth expiry tears the end down (12-5)
#define ADSP_OPEN_RETRY_NS     2000000000ull // open-request retransmit (12-30)
#define ADSP_OPEN_RETRY_LIMIT  4 // client-specified; ours is four tries
#define ADSP_RETRANSMIT_NS     1000000000ull // unacknowledged-data retransmit
#define ADSP_ATTN_RETRY_NS     1000000000ull // attention retransmit (12-21)
#define ADSP_FRESET_RETRY_NS   1000000000ull // forward-reset retransmit (12-10)

// Consecutive out-of-sequence data packets before we send Retransmit Advice.
#define ADSP_OOS_ADVICE_THRESHOLD 3

// === Connection-end state (12-5) ============================================

typedef enum {
    ADSP_STATE_CLOSED = 0, // free slot
    ADSP_STATE_LISTENING, // connection-listening socket (12-35)
    ADSP_STATE_OPENING, // our open request is outstanding
    ADSP_STATE_ESTABLISHED, // established from a received request, no ack yet
    ADSP_STATE_OPEN, // both ends established — data may flow
} adsp_state_t;

// Names published by `appletalk.adsp.connections[i].state`.
extern const char *const ADSP_STATE_NAMES[];
#define ADSP_STATE_COUNT 5

// === Client interface =======================================================
//
// The PPC session layer (and the unit suite) plug in here.  Every callback is
// optional; a NULL `on_accept` means "accept every open request".

typedef struct {
    // A listener was handed an open request from `from`; false denies it.
    bool (*on_accept)(void *ctx, const atalk_socket_addr_t *from);
    // The connection reached ADSP_STATE_OPEN.
    void (*on_open)(void *ctx, adsp_conn_t *c);
    // In-order stream bytes.  `eom` marks the last byte of a client message.
    void (*on_data)(void *ctx, adsp_conn_t *c, const uint8_t *data, int len, bool eom);
    // An attention message arrived (code plus up to ADSP_MAX_ATTN_DATA bytes).
    void (*on_attention)(void *ctx, adsp_conn_t *c, uint16_t code, const uint8_t *data, int len);
    // The connection end is gone; `reason` is a human-readable phrase.
    void (*on_close)(void *ctx, adsp_conn_t *c, const char *reason);
} adsp_client_t;

// === Host interface =========================================================

typedef struct {
    void *ctx; // opaque, handed back to every callback
    // Emulated time, nanoseconds.  Drives every timer in the engine.
    uint64_t (*now_ns)(void *ctx);
    // Put one built ADSP packet on the wire as DDP type 7.  Returns 0 on success.
    int (*send)(void *ctx, const atalk_socket_addr_t *dest, uint8_t src_socket, const uint8_t *pkt, int len);
    // Optional: the engine's earliest deadline changed; re-arm the host timer.
    // UINT64_MAX means "no deadline pending".
    void (*rearm)(void *ctx, uint64_t deadline_ns);
} adsp_config_t;

// === Engine lifecycle =======================================================

adsp_stack_t *adsp_stack_new(const adsp_config_t *cfg);
void adsp_stack_free(adsp_stack_t *s);

// Close every connection end (machine teardown, checkpoint restore, stack
// disable).  Sends close advice where a peer might still be listening.
void adsp_close_all(adsp_stack_t *s, const char *reason);

// === Engine operations ======================================================

// Publish a connection-listening socket (12-35).  One listener per socket.
int adsp_listen(adsp_stack_t *s, uint8_t socket, const adsp_client_t *client, void *client_ctx);
int adsp_unlisten(adsp_stack_t *s, uint8_t socket);

// Open a connection to `dest` from `local_socket`, starting the open dialog.
// Returns the connection end (state OPENING) or NULL if none is available.
adsp_conn_t *adsp_open(adsp_stack_t *s, const atalk_socket_addr_t *dest, uint8_t local_socket,
                       const adsp_client_t *client, void *client_ctx);

// Queue `len` bytes on the connection's send stream.  `eom` appends an
// end-of-message marker, which consumes one sequence number (12-9).  Returns
// the number of bytes queued, or -1 if the connection cannot take them.
int adsp_write(adsp_stack_t *s, adsp_conn_t *c, const uint8_t *data, int len, bool eom);

// Send an attention message (one outstanding at a time, 12-21).
int adsp_send_attention(adsp_stack_t *s, adsp_conn_t *c, uint16_t code, const uint8_t *data, int len);

// Abort delivery of everything still in flight and resynchronise (12-9).
int adsp_forward_reset(adsp_stack_t *s, adsp_conn_t *c);

// Close one end: send close advice, then free the end.
void adsp_close(adsp_stack_t *s, adsp_conn_t *c, const char *reason);

// Feed one received DDP type 7 packet to the engine.
void adsp_input(adsp_stack_t *s, const atalk_socket_addr_t *from, uint8_t dst_socket, const uint8_t *pkt, int len);

// Earliest pending deadline (UINT64_MAX when idle), and the timer pump.
uint64_t adsp_next_deadline(adsp_stack_t *s);
void adsp_run_timers(adsp_stack_t *s);

// === Connection accessors (observability and the PPC layer) =================

int adsp_conn_id(const adsp_conn_t *c);
adsp_state_t adsp_conn_state(const adsp_conn_t *c);
bool adsp_conn_is_open(const adsp_conn_t *c);
const atalk_socket_addr_t *adsp_conn_remote(const adsp_conn_t *c);
uint8_t adsp_conn_local_socket(const adsp_conn_t *c);
uint16_t adsp_conn_local_cid(const adsp_conn_t *c);
uint16_t adsp_conn_remote_cid(const adsp_conn_t *c);
bool adsp_conn_initiator(const adsp_conn_t *c);
uint64_t adsp_conn_bytes_in(const adsp_conn_t *c);
uint64_t adsp_conn_bytes_out(const adsp_conn_t *c);
uint32_t adsp_conn_send_seq(const adsp_conn_t *c);
uint32_t adsp_conn_recv_seq(const adsp_conn_t *c);
uint32_t adsp_conn_send_wdw_seq(const adsp_conn_t *c);
uint16_t adsp_conn_recv_wdw(const adsp_conn_t *c);
int adsp_conn_unacked(const adsp_conn_t *c);
uint64_t adsp_conn_retransmits(const adsp_conn_t *c);

// Slot walk for the object model: slots are stable for a connection's life.
int adsp_conn_slot_max(const adsp_stack_t *s);
adsp_conn_t *adsp_conn_at(adsp_stack_t *s, int slot);

// === Engine counters ========================================================

typedef struct {
    uint64_t packets_in;
    uint64_t packets_out;
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint64_t opens; // connections that reached ADSP_STATE_OPEN
    uint64_t open_denials; // denials we sent plus denials we received
    uint64_t retransmits;
    uint64_t out_of_sequence;
    uint64_t forward_resets;
    uint64_t attentions_in;
    uint64_t attentions_out;
    uint64_t timeouts; // ends torn down by the connection timer
} adsp_stats_t;

const adsp_stats_t *adsp_get_stats(const adsp_stack_t *s);

// === Production instance ====================================================
//
// One stack instance wired to the emulator's DDP layer and scheduler.

void atalk_adsp_init(scheduler_t *scheduler);
void atalk_adsp_shutdown(void);
adsp_stack_t *atalk_adsp_stack(void);

// DDP dispatch hook, called by appletalk.c for DDP type 7.
void atalk_adsp_ddp_in(const ddp_header_t *ddp, const uint8_t *buf, int len);

// Object-model surface: attaches `adsp` under `appletalk`.
void atalk_adsp_install_objects(struct object *parent);
void atalk_adsp_remove_objects(void);

#endif // APPLETALK_ADSP_H
