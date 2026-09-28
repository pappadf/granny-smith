// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_internal.h
// Internal AppleTalk protocol definitions shared between implementation modules.

#ifndef APPLETALK_INTERNAL_H
#define APPLETALK_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The scheduler the stack was initialised with (NULL before appletalk_init),
// for guest-time timers in the protocol modules.
struct scheduler *atalk_scheduler(void);

// The stack's clock: guest time from the machine's scheduler, in ns, so a run
// is deterministic; 0 before a scheduler is attached.  ASP, PAP and ADSP all
// read it -- PAP used to fall back to the host's clock.
uint64_t atalk_now_ns(void);

// A guest-time timer the stack owns -- one scheduler event type.
//
// Every one is registered with the machine's scheduler while the stack comes
// up: atalk_timer_init is called from the owning module's init, which
// appletalk_init reaches.  Never lazily at first arm: a checkpoint restore
// replays the saved event queue into a fresh scheduler before any session
// exists, and a saved event whose type nothing has registered fails the load
// (when ATP, PAP and the LaserWriter job registered at first arm, a
// checkpoint taken during file sharing or printing could not be restored).
// appletalk_teardown forgets every initialised timer.
//
// The scheduler source is the timer itself, so a timer's callback receives
// its own address as `source`.
typedef void (*atalk_timer_fn)(void *source, uint64_t data);
typedef struct atalk_timer {
    atalk_timer_fn cb;
    bool registered; // with the current stack's scheduler
} atalk_timer_t;

// Register `t` as "source_name.event_name" with the stack's scheduler.  Call
// from the owning module's init; repeat calls are harmless.
void atalk_timer_init(atalk_timer_t *t, const char *source_name, const char *event_name, atalk_timer_fn cb);
// One-shot `delay_ns` from now, carrying `data`; replaces a pending event of
// this timer with the same `data`, so distinct data (one per ATP transaction)
// can be pending together.  Delays under ATALK_TIMER_MIN_NS are raised to it:
// a zero or sub-cycle delay fires with the clock unchanged and a timer that
// re-arms itself would spin.
void atalk_timer_arm(atalk_timer_t *t, uint64_t data, uint64_t delay_ns);
// Cancel the pending event carrying `data`, or every pending event.
void atalk_timer_cancel(atalk_timer_t *t, uint64_t data);
void atalk_timer_cancel_all(atalk_timer_t *t);

#define ATALK_TIMER_MIN_NS 1000u

// Shared AppleTalk constants
#define LLAP_HOST_NODE         33
#define HOST_AFP_SOCKET        8
#define HOST_AFP_COMPAT_SOCKET 54
#define HOST_PAP_SOCKET        6

// DDP protocol type field values (Inside AppleTalk 4-11).  ADSP is 7 — the
// stack doc claimed 10 until the ADSP work corrected it.
#define DDP_TYPE_RTMP_RESPONSE 0x01
#define DDP_TYPE_NBP           0x02
#define DDP_TYPE_ATP           0x03
#define DDP_TYPE_AEP           0x04
#define DDP_TYPE_RTMP_REQUEST  0x05
#define DDP_TYPE_ZIP           0x06
#define DDP_TYPE_ADSP          0x07

#define LLAP_HEADER_SIZE         3
#define DDP_SHORT_HEADER_SIZE    5
#define DDP_EXTENDED_HEADER_SIZE 13
#define DDP_MAX_DATA_SIZE        586

// ATP control bit masks (ctl field upper bits per Inside AppleTalk 10-7)
// ATP limits: a response is at most eight packets (Inside AppleTalk 9-8) of
// at most 578 bytes of data each.
#define ATP_MAX_RESPONSE_FRAGMENTS 8
#define ATP_MAX_ATP_PAYLOAD        578

#define ATP_CONTROL_TREQ  0x40
#define ATP_CONTROL_TRESP 0x80
#define ATP_CONTROL_TREL  0xC0
#define ATP_CONTROL_XO    0x20
#define ATP_CONTROL_EOM   0x10
#define ATP_CONTROL_STS   0x08

// PAP function selectors carried in ATP User Byte 2
#define PAP_FUNC_OPEN        1
#define PAP_FUNC_OPEN_REPLY  2
#define PAP_FUNC_SENDDATA    3
#define PAP_FUNC_DATA        4
#define PAP_FUNC_TICKLE      5
#define PAP_FUNC_CLOSE       6
#define PAP_FUNC_CLOSE_REPLY 7
#define PAP_FUNC_SEND_STATUS 8
#define PAP_FUNC_STATUS      9

#define PAP_RESULT_OK        0x0000
#define PAP_RESULT_BUSY      0xFFFF
#define PAP_MAX_FLOW_QUANTUM 8
#define PAP_MAX_DATA_SIZE    512

// LLAP framing header
typedef struct {
    uint8_t dst;
    uint8_t src;
    uint8_t type;
} llap_header_t;

// DDP short header (subset of extended fields for our use)
typedef struct {
    llap_header_t llap;
    uint8_t hop;
    uint16_t len;
    uint16_t checksum;
    uint16_t dst_net;
    uint16_t src_net;
    uint8_t dst_socket;
    uint8_t src_socket;
    uint8_t type;
} ddp_header_t;

// Parsed ATP frame used internally by the stack
typedef struct {
    uint8_t ctl;
    uint8_t bitmap;
    uint16_t tid;
    uint8_t user[4];
    const uint8_t *data;
    int data_len;
} atp_packet_t;

// Minimal address descriptor for targeting remote AppleTalk sockets
typedef struct {
    uint16_t net;
    uint8_t node;
    uint8_t socket;
} atalk_socket_addr_t;

// ATP public API (implemented in appletalk.c)
typedef enum { ATP_TRANSACTION_ALO = 0, ATP_TRANSACTION_XO = 1 } atp_transaction_mode_t;

typedef enum { ATP_REQUEST_RESULT_OK = 0, ATP_REQUEST_RESULT_TIMEOUT, ATP_REQUEST_RESULT_ABORTED } atp_request_result_t;

typedef struct {
    uint8_t seq;
    bool duplicate;
    bool eom;
    bool sts;
    uint8_t bitmap_remaining;
    uint8_t user[4];
    const uint8_t *data;
    int data_len;
} atp_response_fragment_t;

typedef struct atp_request_handle atp_request_handle_t;

typedef struct {
    void (*on_response)(const atp_response_fragment_t *fragment, void *ctx);
    void (*on_complete)(atp_request_handle_t *handle, atp_request_result_t result, void *ctx);
} atp_request_callbacks_t;

typedef struct {
    atalk_socket_addr_t dest;
    uint8_t src_socket;
    uint8_t bitmap;
    atp_transaction_mode_t mode;
    uint8_t trel_timer_hint;
    uint8_t user[4];
    const uint8_t *payload;
    int payload_len;
    uint32_t retry_timeout_ms;
    int retry_limit; // < 0 => infinite retries
} atp_request_params_t;

atp_request_handle_t *atp_request_submit(const atp_request_params_t *params, const atp_request_callbacks_t *callbacks,
                                         void *ctx);
void atp_request_cancel(atp_request_handle_t *handle);

typedef struct {
    const uint8_t *payload;
    int payload_len;
    const uint8_t *user;
    bool sts;
    bool eom;
} atp_response_packet_desc_t;

typedef struct {
    void (*handle_request)(const ddp_header_t *ddp, atp_packet_t *request, void *ctx);
} atp_socket_handler_t;

int atp_register_socket_handler(uint8_t socket, const atp_socket_handler_t *handler, void *ctx);
void atp_unregister_socket_handler(uint8_t socket);

int atp_responder_send_packets(const ddp_header_t *request_ddp, const atp_packet_t *request_atp,
                               const atp_response_packet_desc_t *packets, size_t packet_count);

int atp_responder_send_simple(const ddp_header_t *request_ddp, const atp_packet_t *request_atp, const uint8_t user[4],
                              const uint8_t *payload, int payload_len, bool sts);

// Send one datagram to a remote AppleTalk socket.  The DDP/LLAP headers are
// built here; `data` is the protocol payload (for ADSP, its 13-byte header
// plus body).  Returns 0 on success, -1 if the stack is detached or the
// payload does not fit a DDP packet.
int atalk_ddp_send_to(const atalk_socket_addr_t *dest, uint8_t src_socket, uint8_t ddp_type, const uint8_t *data,
                      int len);

// Printer AppleTalk entry points.  register runs each time the stack comes
// up; shutdown pairs it when the stack goes away with its machine (the
// session, the job and the advertisement go; the configuration -- enabled,
// name, capture -- stays for the next stack); link_down drops the session
// when the stack is detached from the link, since its client is unreachable.
void atalk_printer_register(void);
void atalk_printer_shutdown(void);
void atalk_printer_link_down(void);

// Publish (or rename) / withdraw the LaserWriter NBP entity.  The object model
// drives these through atalk_printer_set_enabled / atalk_printer_set_name.
int atalk_printer_enable(const char *object_name);
int atalk_printer_disable(void);

#endif // APPLETALK_INTERNAL_H
