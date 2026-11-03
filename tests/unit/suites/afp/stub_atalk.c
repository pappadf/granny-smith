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
#include "macroman.h"

#include <stdio.h>

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
// The NBP name rule (appletalk.c): a name MacRoman can hold, 1 to 32 bytes.
int atalk_nbp_name_check(const char *what, const char *name, char *err, size_t err_len) {
    uint8_t mac[ATALK_NBP_NAME_MAX * 4];
    int n = macroman_from_utf8(name ? name : "", mac, sizeof(mac));
    if (n <= 0 || n > ATALK_NBP_NAME_MAX) {
        snprintf(err, err_len, "%s refused", what);
        return -1;
    }
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

// rename() as the server sees it (the suite links with --wrap=rename).  With
// stub_refuse_dir_rename set, a directory is refused as WasmFS's OPFS backend
// refuses it, which renames files only (S1018-23).
#include <errno.h>
#include <sys/stat.h>

int stub_refuse_dir_rename;
int __real_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to) {
    struct stat st;
    if (stub_refuse_dir_rename && stat(from, &st) == 0 && S_ISDIR(st.st_mode)) {
        errno = EXDEV;
        return -1;
    }
    return __real_rename(from, to);
}

// ---- Further servers and remote volumes ------------------------------------
//
// asp_add_service keeps the client and context each further server registers,
// so the suite can drive that server the way ASP would: through the client.
#include "afp_remote.h"
#include "event/event.h"
#include "io/io_worker.h"

#include <stdlib.h>

const asp_client_t *stub_service_client[ASP_MAX_SERVICES];
void *stub_service_ctx[ASP_MAX_SERVICES];
int stub_retries; // asp_retry_deferred calls
char stub_last_event[2048]; // the last event the core emitted

int asp_add_service(uint8_t socket, const asp_client_t *client, void *ctx) {
    (void)socket;
    for (int i = 1; i < ASP_MAX_SERVICES; i++)
        if (!stub_service_client[i]) {
            stub_service_client[i] = client;
            stub_service_ctx[i] = ctx;
            return i;
        }
    return -1;
}
void asp_remove_service(int service) {
    if (service > 0 && service < ASP_MAX_SERVICES) {
        stub_service_client[service] = NULL;
        stub_service_ctx[service] = NULL;
    }
}
void asp_service_broadcast_attention(int service, uint16_t code) {
    (void)service;
    (void)code;
    g_attentions++;
}
void asp_service_close_sessions(int service) {
    (void)service;
}
void asp_retry_deferred(void) {
    stub_retries++;
}

// No I/O worker: the core does the work inline.
uint32_t io_submit_work(io_work_fn work, void *ud, io_done_fn done, void *dud) {
    (void)work, (void)ud, (void)done, (void)dud;
    return 0;
}

// No VFS: nothing unpacks, so a container shows as its file.
int shell_cp_contents(const char *src, const char *dst, uint64_t *files, uint64_t *bytes, char *err_buf,
                      size_t err_cap) {
    (void)src, (void)dst, (void)bytes;
    if (files)
        *files = 0;
    snprintf(err_buf, err_cap, "no VFS in this suite");
    return -38; // ENOSYS
}

void event_emit(event_kind_t kind, const char *json) {
    (void)kind;
    snprintf(stub_last_event, sizeof stub_last_event, "%s", json ? json : "");
}
