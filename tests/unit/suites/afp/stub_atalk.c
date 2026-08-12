// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Transport stubs for the AFP wire suite.
//
// appletalk_server.c calls into the AppleTalk stack for NBP registration and
// for the ASP session view.  The suite drives AFP commands directly, so those
// calls are answered here instead of linking the whole stack: NBP succeeds
// and records nothing, and the session table is a single entry whose
// negotiated AFP version the tests set.

#include "appletalk.h"

#include <string.h>

static char g_version[24] = "AFPVersion 2.1";
static int g_attentions;

// Let a test choose what the session negotiated (2.0 gates off the 2.1 calls).
void stub_set_afp_version(const char *v) {
    snprintf(g_version, sizeof(g_version), "%s", v ? v : "");
}
int stub_attention_count(void) {
    return g_attentions;
}

int atalk_nbp_register(const atalk_nbp_service_desc_t *desc, atalk_nbp_entry_t **out_entry) {
    (void)desc;
    if (out_entry)
        *out_entry = (atalk_nbp_entry_t *)(void *)&g_version; // any non-NULL handle
    return 0;
}
int atalk_nbp_update(atalk_nbp_entry_t *entry, const atalk_nbp_service_desc_t *desc) {
    (void)entry;
    (void)desc;
    return 0;
}
int atalk_nbp_unregister(atalk_nbp_entry_t *entry) {
    (void)entry;
    return 0;
}

void atalk_asp_session_set_afp_version(uint16_t session_ref, const char *version) {
    (void)session_ref;
    stub_set_afp_version(version);
}
const char *atalk_asp_session_afp_version(uint16_t session_ref) {
    (void)session_ref;
    return g_version;
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
