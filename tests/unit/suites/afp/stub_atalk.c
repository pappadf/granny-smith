// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Transport stubs for the AFP wire suite.
//
// appletalk_server.c calls into the AppleTalk stack for NBP registration,
// ASP client registration and attentions.  The suite drives AFP commands
// directly, so those calls are answered here instead of linking the whole
// stack.  Sessions are the server's own (afp_session_opened); the suite opens
// them itself, as ASP would.

#include "appletalk.h"
#include "appletalk_asp.h"

#include <string.h>

static int g_attentions;

int stub_attention_count(void) {
    return g_attentions;
}

// ASP's client registration: the suite calls the server directly.
void asp_set_client(const asp_client_t *client, void *ctx) {
    (void)client, (void)ctx;
}

// One registry of one entity: "Taken" is refused, as another machine holding
// the name would make it (the rename tests).
int atalk_nbp_publish(atalk_nbp_entry_t **entry, const atalk_nbp_service_desc_t *desc) {
    if (!entry || !desc || (desc->object && strcmp(desc->object, "Taken") == 0))
        return -1;
    *entry = (atalk_nbp_entry_t *)(void *)&g_attentions; // any non-NULL handle
    return 0;
}
void atalk_nbp_withdraw(atalk_nbp_entry_t **entry) {
    if (entry)
        *entry = NULL;
}

int atalk_asp_send_attention(uint16_t session_ref, uint16_t code) {
    (void)session_ref;
    (void)code;
    g_attentions++;
    return 0;
}
void atalk_asp_broadcast_attention(uint16_t code) {
    (void)code;
    g_attentions++;
}
void atalk_asp_close_all_sessions(void) {}
