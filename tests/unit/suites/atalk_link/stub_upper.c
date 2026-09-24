// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// stub_upper.c
// Everything above the link and transport layers that appletalk.c links
// against: the AFP server, the printer, ADSP, PPC and Apple events.  The
// length of this file is the measure of 10-network F-21: to test LLAP, DDP,
// NBP, ATP and ASP you have to stand in for all of them.
//
// afp_handle_command records what reached it, so ASP tests can see which
// session and opcode a command was dispatched as.

#include "stub_upper.h"

#include "appletalk.h"
#include "appletalk_adsp.h"
#include "appletalk_aevt.h"
#include "appletalk_asp.h"
#include "appletalk_internal.h"
#include "appletalk_ppc.h"

#include <stdio.h>
#include <string.h>

void LOG_INDENT(int n) {
    (void)n;
}

// ---- AFP server (appletalk_server.c) --------------------------------------
//
// atalk_server_init registers an ASP client that records what reached it, so
// ASP tests can see which session and opcode a command was dispatched as.
int g_afp_calls;
uint8_t g_afp_last_opcode;
uint16_t g_afp_last_session;
int g_asp_closes;

static uint32_t rec_command(void *ctx, uint16_t session_ref, uint8_t opcode, const uint8_t *in, int in_len,
                            uint8_t *out, int out_max, int *out_len) {
    (void)ctx, (void)in, (void)in_len, (void)out, (void)out_max;
    g_afp_calls++;
    g_afp_last_opcode = opcode;
    g_afp_last_session = session_ref;
    *out_len = 0;
    return 0;
}
static void rec_close(void *ctx, uint16_t session_ref) {
    (void)ctx, (void)session_ref;
    g_asp_closes++;
}
static const asp_client_t k_rec_client = {.on_close = rec_close, .on_command = rec_command};

void atalk_server_init(void) {
    asp_set_client(&k_rec_client, NULL);
}
const char *const *atalk_afp_versions(int *c) {
    *c = 0;
    return NULL;
}
static atalk_afp_stats_t g_afp_stats;
const atalk_afp_stats_t *atalk_afp_get_stats(void) {
    return &g_afp_stats;
}
int atalk_afp_error_code_at(int i, int32_t *c, uint64_t *n) {
    (void)i, (void)c, (void)n;
    return -1;
}
unsigned atalk_afp_volume_open_forks(int s) {
    (void)s;
    return 0;
}
unsigned atalk_afp_volume_sessions_using(int s) {
    (void)s;
    return 0;
}
unsigned atalk_afp_volume_catalog_generation(int s) {
    (void)s;
    return 0;
}
unsigned atalk_afp_volume_cnid_count(int s) {
    (void)s;
    return 0;
}

// ---- printer (appletalk_printer.c) ----------------------------------------
void atalk_printer_register(void) {}
void atalk_printer_shutdown(void) {}
void atalk_printer_link_down(void) {}
const char *atalk_printer_status_text(void) {
    return "";
}
bool atalk_printer_has_interpreter(void) {
    return false;
}
const atalk_printer_stats_t *atalk_printer_get_stats(void) {
    static atalk_printer_stats_t none;
    return &none;
}
uint32_t atalk_printer_documents(void) {
    return 0;
}
uint32_t atalk_printer_last_pages(void) {
    return 0;
}
const char *atalk_printer_last_outcome(void) {
    return "";
}

// ---- ADSP / PPC / AEVT -------------------------------------------------------
void atalk_adsp_init(scheduler_t *s) {
    (void)s;
}
void atalk_adsp_shutdown(void) {}
adsp_stack_t *atalk_adsp_stack(void) {
    return NULL;
}
void adsp_close_all(adsp_stack_t *s, const char *r) {
    (void)s, (void)r;
}
int g_adsp_in_calls;
int g_adsp_in_last_len;
void atalk_adsp_ddp_in(const ddp_header_t *ddp, const uint8_t *buf, int len) {
    (void)ddp, (void)buf;
    g_adsp_in_calls++;
    g_adsp_in_last_len = len;
}
void atalk_adsp_install_objects(struct object *p) {
    (void)p;
}
void atalk_adsp_remove_objects(void) {}
void atalk_ppc_init(void) {}
void atalk_ppc_shutdown(void) {}
void atalk_ppc_close_all(const char *r) {
    (void)r;
}
void atalk_ppc_install_objects(struct object *p) {
    (void)p;
}
void atalk_ppc_remove_objects(void) {}
atalk_aevt_config_t g_aevt_restored;
int g_aevt_set_calls;
void atalk_aevt_init(void) {}
void atalk_aevt_shutdown(void) {}
void atalk_aevt_get_config(atalk_aevt_config_t *o) {
    memset(o, 0, sizeof(*o));
}
void atalk_aevt_set_config(const atalk_aevt_config_t *in) {
    g_aevt_set_calls++;
    g_aevt_restored = *in;
}
void atalk_aevt_reset_transient_state(void) {}
void atalk_aevt_install_objects(struct object *p) {
    (void)p;
}
void atalk_aevt_remove_objects(void) {}

// ---- configuration the stack captures and restores (10-network A5) -----------
//
// Stateful, like the real modules: a checkpoint round trip can be checked.
// Server identity and printer settings are process-wide and survive a
// teardown; the volume table does not (atalk_server_delete empties it).

static char g_afp_name[33] = "Test Server";
static char g_afp_message[200];
static bool g_afp_enabled = true;
static char g_printer_name[33] = "LaserWriter";
static bool g_printer_enabled = true;
static bool g_printer_capture;
static struct {
    bool in_use;
    char name[33];
    char path[256];
    unsigned vol_id;
} g_stub_vols[8];
static unsigned g_stub_next_vol_id = 1;

void atalk_server_delete(void) {
    memset(g_stub_vols, 0, sizeof(g_stub_vols));
}
const char *atalk_afp_get_name(void) {
    return g_afp_name;
}
int atalk_afp_set_name(const char *n, char *e, size_t el) {
    (void)e, (void)el;
    snprintf(g_afp_name, sizeof(g_afp_name), "%s", n);
    return 0;
}
bool atalk_afp_get_enabled(void) {
    return g_afp_enabled;
}
int atalk_afp_set_enabled(bool en, char *e, size_t el) {
    (void)e, (void)el;
    g_afp_enabled = en;
    return 0;
}
const char *atalk_afp_get_message(void) {
    return g_afp_message;
}
int atalk_afp_set_message(const char *m, char *e, size_t el) {
    (void)e, (void)el;
    snprintf(g_afp_message, sizeof(g_afp_message), "%s", m);
    return 0;
}
int atalk_afp_volume_restore(const char *n, const char *p, unsigned vol_id, char *e, size_t el) {
    for (int i = 0; i < 8; i++)
        if (g_stub_vols[i].in_use && (g_stub_vols[i].vol_id == vol_id || strcmp(g_stub_vols[i].name, n) == 0)) {
            snprintf(e, el, "taken");
            return -1;
        }
    for (int i = 0; i < 8; i++)
        if (!g_stub_vols[i].in_use) {
            g_stub_vols[i].in_use = true;
            snprintf(g_stub_vols[i].name, sizeof(g_stub_vols[i].name), "%s", n);
            snprintf(g_stub_vols[i].path, sizeof(g_stub_vols[i].path), "%s", p);
            g_stub_vols[i].vol_id = vol_id;
            if (vol_id >= g_stub_next_vol_id)
                g_stub_next_vol_id = vol_id + 1;
            return i;
        }
    snprintf(e, el, "full");
    return -1;
}
int atalk_afp_volume_add(const char *n, const char *p, char *e, size_t el) {
    return atalk_afp_volume_restore(n, p, g_stub_next_vol_id, e, el);
}
int atalk_afp_volume_remove(const char *n, char *e, size_t el) {
    (void)e, (void)el;
    for (int i = 0; i < 8; i++)
        if (g_stub_vols[i].in_use && strcmp(g_stub_vols[i].name, n) == 0) {
            g_stub_vols[i].in_use = false;
            return 0;
        }
    return -1;
}
int atalk_afp_volume_max(void) {
    return 8;
}
int atalk_afp_volume_find(const char *n) {
    for (int i = 0; i < 8; i++)
        if (g_stub_vols[i].in_use && strcmp(g_stub_vols[i].name, n) == 0)
            return i;
    return -1;
}
bool atalk_afp_volume_in_use(int s) {
    return s >= 0 && s < 8 && g_stub_vols[s].in_use;
}
const char *atalk_afp_volume_name(int s) {
    return g_stub_vols[s].name;
}
const char *atalk_afp_volume_path(int s) {
    return g_stub_vols[s].path;
}
unsigned atalk_afp_volume_vol_id(int s) {
    return g_stub_vols[s].vol_id;
}
bool atalk_printer_is_enabled(void) {
    return g_printer_enabled;
}
const char *atalk_printer_object_name(void) {
    return g_printer_name;
}
int atalk_printer_set_enabled(bool en, char *e, size_t el) {
    (void)e, (void)el;
    g_printer_enabled = en;
    return 0;
}
int atalk_printer_set_name(const char *n, char *e, size_t el) {
    (void)e, (void)el;
    snprintf(g_printer_name, sizeof(g_printer_name), "%s", n);
    return 0;
}
bool atalk_printer_capture_get(void) {
    return g_printer_capture;
}
void atalk_printer_capture_set(bool en) {
    g_printer_capture = en;
}
