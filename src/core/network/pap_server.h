// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pap_server.h
// The server side of the Printer Access Protocol that both of the host's
// printers speak -- the LaserWriter (appletalk_printer.c) and the
// ImageWriter's LocalTalk Option card (appletalk_imagewriter.c): the shared
// timing, the status-reply prefix, the OpenConn acceptance rule, the
// SendData and CloseConn requests and the session predicate, once.  Each
// printer keeps its own session state, data sink and status payload.
//
// Sources: Inside AppleTalk, 2nd ed., ch. 10 (PAP); Technical Note NW20.

#ifndef GS_PAP_SERVER_H
#define GS_PAP_SERVER_H

#include "appletalk_internal.h"

#include <stdbool.h>
#include <stdint.h>

// The connection timer: a workstation silent this long is gone.
#define PAP_SESSION_TIMEOUT_NS (120ull * 1000000000ull)
// Guest time between a SendData transaction's end and the next one.
#define PAP_SENDDATA_GAP_NS 10000000ull
// A SendData's ATP retry interval.
#define PAP_SENDDATA_RETRY_MS 8000u

// The first four bytes of an OpenConnReply / Status: the server's socket,
// its flow quantum and the result word, big-endian.  The status data follows.
void pap_status_prefix(uint8_t out[4], uint8_t socket, uint8_t quantum, uint16_t result);

// Whether an OpenConn is to be answered at all.  Its result is OK or
// PrinterBusy -- the spec has no other refusal -- and a workstation answers
// busy by retrying, without end.  So a printer that is not advertised stays
// silent, as one switched off would, and a malformed request (less than its
// 4 bytes: socket, flow quantum, wait time) is dropped: either way the
// workstation's open times out with an error.
bool pap_open_answerable(bool advertised, const atp_packet_t *open);

// The workstation's flow quantum from an OpenConn: 1..8 by spec (one ATP
// response is at most eight packets); 0 or more is taken as the maximum.
uint8_t pap_open_quantum(const atp_packet_t *open);

// The next SendData sequence number from *next, which wraps past 0 (0 means
// "unsequenced" in PAP).
uint16_t pap_take_seq(uint16_t *next);

// True when a request belongs to the connection `conn_id` opened from
// `client`: its connection id, from the node and network that opened it.  The
// id is one byte the workstation chose, so on its own it is anyone's.
bool pap_request_is_session(bool active, uint8_t conn_id, const atalk_socket_addr_t *client, const ddp_header_t *ddp,
                            const atp_packet_t *atp);

// Pull the next `quantum` packets from the workstation: a SendData request
// (XO, SendData's release hint) for sequence `seq`, retried every
// PAP_SENDDATA_RETRY_MS up to `retry_limit` times (-1: without end).
atp_request_handle_t *pap_submit_senddata(const atalk_socket_addr_t *client, uint8_t src_socket, uint8_t conn_id,
                                          uint8_t quantum, uint16_t seq, int retry_limit,
                                          const atp_request_callbacks_t *cb, void *ctx);

// Ask the workstation to close connection `conn_id`.  Exactly-once, as every
// PAP request but Tickle is: the workstation's PAP keeps the CloseConnReply
// for a retransmission, which an ALO request would make it act on twice.  The
// release hint is SendData's; retried every 2 s, five times.  `cb` may be NULL.
atp_request_handle_t *pap_submit_close_conn(const atalk_socket_addr_t *client, uint8_t src_socket, uint8_t conn_id,
                                            const atp_request_callbacks_t *cb, void *ctx);

#endif // GS_PAP_SERVER_H
