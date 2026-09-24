// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The PPC session layer beneath the Apple-event endpoint, stubbed: one open
// session a test can deliver events on, a count of the blocks written to it
// (the endpoint's replies), and an open that always fails -- no port is ever
// found -- so outbound events fail at once but keep their slot.

#include "stub_ppc.h"
#include "appletalk_ppc.h"

#include <stdio.h>
#include <string.h>

struct ppc_session {
    int id;
};

static struct ppc_session g_session = {.id = 0x12345}; // wider than 16 bits (N-24)
int stub_ppc_blocks;
const ppc_session_t *stub_ppc_last_block_session;

ppc_session_t *stub_ppc_session(void) {
    return &g_session;
}

void stub_ppc_reset(void) {
    stub_ppc_blocks = 0;
    stub_ppc_last_block_session = NULL;
}

int atalk_ppc_set_host_port(const char *name, bool enabled, char *err, size_t err_len) {
    (void)name;
    (void)enabled;
    (void)err;
    (void)err_len;
    return 0;
}

void atalk_ppc_set_inbound_client(const ppc_client_t *client, void *ctx) {
    (void)client;
    (void)ctx;
}

ppc_session_t *atalk_ppc_open(const char *port_name, const ppc_client_t *client, void *ctx, char *err, size_t err_len) {
    (void)client;
    (void)ctx;
    snprintf(err, err_len, "no port '%s' on this wire", port_name ? port_name : "");
    return NULL;
}

int atalk_ppc_send_block(ppc_session_t *s, uint32_t creator, uint32_t type, uint32_t user_data, const uint8_t *payload,
                         int len) {
    (void)creator;
    (void)type;
    (void)user_data;
    (void)payload;
    (void)len;
    stub_ppc_blocks++;
    stub_ppc_last_block_session = s;
    return 0;
}

void atalk_ppc_close(ppc_session_t *s, const char *reason) {
    (void)s;
    (void)reason;
}

ppc_session_state_t atalk_ppc_session_state(const ppc_session_t *s) {
    (void)s;
    return PPC_SESSION_OPEN;
}

const char *atalk_ppc_session_port(const ppc_session_t *s) {
    (void)s;
    return "guest";
}
