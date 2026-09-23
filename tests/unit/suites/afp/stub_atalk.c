// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Transport stubs for the AFP wire suite.
//
// appletalk_server.c calls into the AppleTalk stack for NBP registration and
// for the ASP session view.  The suite drives AFP commands directly, so those
// calls are answered here instead of linking the whole stack: NBP succeeds
// and records nothing, and the ASP session table is a small table keyed by
// session ref, as the real one is.  A session the table does not hold has no
// negotiated version, exactly as atalk_asp_session_afp_version answers for an
// unknown ref -- so a test can drive a second session, or one that never
// opened, and see what the server does with it (10-network unit 0.3).

#include "appletalk.h"

#include <string.h>

#define STUB_SESSIONS 8
#define STUB_DEFAULT  0x0021 // the SESSION every test.c call uses

static struct {
    uint16_t ref;
    char version[24];
} g_sessions[STUB_SESSIONS];
static int g_attentions;

static int session_slot(uint16_t ref, bool create) {
    for (int i = 0; i < STUB_SESSIONS; i++)
        if (g_sessions[i].ref == ref && ref != 0)
            return i;
    if (!create || ref == 0)
        return -1;
    for (int i = 0; i < STUB_SESSIONS; i++)
        if (g_sessions[i].ref == 0) {
            g_sessions[i].ref = ref;
            g_sessions[i].version[0] = '\0';
            return i;
        }
    return -1;
}

// Open `ref` with version `v` (NULL or "" closes it).
void stub_session_set(uint16_t ref, const char *v) {
    int i = session_slot(ref, v && *v);
    if (i < 0)
        return;
    if (!v || !*v) {
        g_sessions[i].ref = 0;
        return;
    }
    snprintf(g_sessions[i].version, sizeof(g_sessions[i].version), "%s", v);
}

// Let a test choose what the default session negotiated (2.0 gates off the
// 2.1 calls).
void stub_set_afp_version(const char *v) {
    stub_session_set(STUB_DEFAULT, v);
}
int stub_attention_count(void) {
    return g_attentions;
}

int atalk_nbp_register(const atalk_nbp_service_desc_t *desc, atalk_nbp_entry_t **out_entry) {
    (void)desc;
    if (out_entry)
        *out_entry = (atalk_nbp_entry_t *)(void *)&g_sessions; // any non-NULL handle
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
    stub_session_set(session_ref, version);
}
const char *atalk_asp_session_afp_version(uint16_t session_ref) {
    int i = session_slot(session_ref, false);
    return i < 0 ? NULL : g_sessions[i].version;
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
