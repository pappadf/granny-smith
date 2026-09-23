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
    // Forks a session holds, for the session view.
    uint32_t (*open_forks)(void *ctx, uint16_t session_ref);
} asp_client_t;

// Install the client (NULL removes it).  The AFP server does this from
// atalk_server_init.
void asp_set_client(const asp_client_t *client, void *ctx);

// Called by appletalk_init: forget every session, register the session sweep
// timer, and take the AFP sockets.
void asp_init(void);
// Called by appletalk_teardown, after the client is gone: forget every session
// and the pending write without calling back.
void asp_shutdown(void);

// Session numbering, carried in the stack's checkpoint record.
uint16_t asp_next_ref(void);
void asp_set_next_ref(uint16_t ref);

#endif // APPLETALK_ASP_H
