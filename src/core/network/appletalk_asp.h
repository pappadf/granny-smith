// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_asp.h
// ASP, the AppleTalk Session Protocol (Inside AppleTalk ch. 11): the server
// end of sessions on the AFP sockets (8, and 54 for compatibility).  ASP owns
// the sessions, their tickle expiry, attentions and the two-transaction
// SPWrite; what a command means belongs to the client -- the AFP server -- which
// registers itself here.  The public session view (atalk_asp_*) is in
// appletalk.h.

#ifndef APPLETALK_ASP_H
#define APPLETALK_ASP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The layer above ASP.  Every callback is optional.
typedef struct {
    // A workstation asks to open a session; return false to refuse it (the
    // workstation hears ServerBusy).  NULL accepts every session.
    bool (*on_open)(void *ctx, uint16_t session_ref);
    // A session closed -- the client asked, it expired, or the stack is
    // detaching: release what it held.
    void (*on_close)(void *ctx, uint16_t session_ref);
    // One command, or an SPWrite's command with its data appended.  Returns
    // the 32-bit result that travels back in the ATP user bytes; the reply
    // payload goes in `out` (at most `out_max` bytes), its length in *out_len.
    uint32_t (*on_command)(void *ctx, uint16_t session_ref, uint8_t opcode, const uint8_t *in, int in_len, uint8_t *out,
                           int out_max, int *out_len);
    // The service status block SPGetStatus returns (malloc'd; ASP frees it).
    // 0 on success.
    int (*get_status)(void *ctx, uint8_t **out, size_t *out_len);
    // Forks a session holds, and the AFP version it logged in with, for the
    // session view.
    uint32_t (*open_forks)(void *ctx, uint16_t session_ref);
    const char *(*session_version)(void *ctx, uint16_t session_ref);
} asp_client_t;

// Install the client (NULL removes it) of the first service, the one on the
// AFP sockets 8 and 54.  The AFP server does this from atalk_server_init.
void asp_set_client(const asp_client_t *client, void *ctx);

// Further services: each a server socket of its own (the SSS a workstation's
// OpenSess learns) with its own client, so several file servers can share one
// node.  Returns the service number, or -1 when the socket is taken or the
// table is full.  Removing a service closes its sessions.
#define ASP_MAX_SERVICES 4
int asp_add_service(uint8_t socket, const asp_client_t *client, void *ctx);
void asp_remove_service(int service);

// One service's sessions: send each an attention, or close them all.
void asp_service_broadcast_attention(int service, uint16_t code);
void asp_service_close_sessions(int service);

// A command's result that is not one: the client cannot answer yet (its data
// is on the way) and will run the command again.  ASP keeps the request and
// replays it on every asp_retry_deferred until it gets a real result; the
// workstation's retransmissions of it are absorbed meanwhile.  No AFP result
// is positive, so the value cannot collide with one.
#define ASP_RESULT_DEFERRED 0x7FFFFFFFu

// Replay every deferred command (the client's data arrived).  Safe to call
// from inside a command: the replay then runs once that command is answered.
void asp_retry_deferred(void);

// Called once, when the network comes up: take the AFP sockets.  Returns
// ASP's part of the network (the client it serves), which the network owns.
typedef struct asp_server asp_server_t;
asp_server_t *asp_init(void);

// ASP's part of a machine's connection (atalk_conn_t): the sessions with
// that Mac and the session numbering.
typedef struct asp_link asp_link_t;

asp_link_t *asp_link_new(void);
void asp_link_free(asp_link_t *link);

// Register `link`'s session sweep timer with `conn`'s machine's scheduler,
// while the connection is being built.
struct atalk_conn;
void asp_link_register_timers(struct atalk_conn *conn, asp_link_t *link);

// Serve `link`'s sessions: the connection was plugged in.  NULL when it is
// unplugged, after the sessions were closed: forget what is left -- a write
// waiting for its data -- without calling back.  The numbering stays with the
// link, so a connection plugged in again does not reissue a reference.
void asp_plug(asp_link_t *link);

// Session numbering, carried in the connection's checkpoint block: the next
// session reference and the next one-byte session id the wire carries.  A
// restored connection continues from both, so a request on a session the
// guest held before the restore names no session and is refused (SessClosed)
// rather than taken for a new session's.
void asp_link_numbering(const asp_link_t *link, uint16_t *next_ref, uint8_t *next_id);
void asp_link_set_numbering(asp_link_t *link, uint16_t next_ref, uint8_t next_id);

#endif // APPLETALK_ASP_H
