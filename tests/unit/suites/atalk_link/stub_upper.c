// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// stub_upper.c
// Everything above the link and transport layers that appletalk.c links
// against: the AFP server, the printer, ADSP, PPC and Apple events.  The
// length of this file is the measure of the coupling: to test LLAP, DDP, NBP,
// ATP and ASP you have to stand in for all of them.
//
// afp_handle_command records what reached it, so ASP tests can see which
// session and opcode a command was dispatched as.

#include "stub_upper.h"

#include "afp_server.h"
#include "appletalk.h"
#include "appletalk_adsp.h"
#include "appletalk_aevt.h"
#include "appletalk_asp.h"
#include "appletalk_internal.h"
#include "appletalk_ppc.h"

#include <stdio.h>
#include <string.h>

// Every part and link the network hands out here is this one dummy.
static int g_link_dummy;

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

afp_server_t *atalk_server_init(void) {
    asp_set_client(&k_rec_client, NULL);
    return (afp_server_t *)(void *)&g_link_dummy;
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
int atalk_afp_ok_command_at(int i, const char **name, uint64_t *n) {
    (void)i, (void)name, (void)n;
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

// ---- the connection's parts above ASP -----------------------------------------
//
// Each layer's part of a connection is opaque to appletalk.c: any non-NULL
// handle will do, and plugging one in or out is counted.
int g_printer_registers;
int g_printer_timer_registrations;
int g_printer_unplugs;

afp_link_t *afp_link_new(void) {
    return (afp_link_t *)(void *)&g_link_dummy;
}
void afp_link_free(afp_link_t *link) {
    (void)link;
}
void afp_plug(afp_link_t *link) {
    (void)link;
}
adsp_link_t *atalk_adsp_link_new(void) {
    return (adsp_link_t *)(void *)&g_link_dummy;
}
void atalk_adsp_link_free(adsp_link_t *link) {
    (void)link;
}
void atalk_adsp_link_register_timers(struct atalk_conn *conn, adsp_link_t *link) {
    (void)conn;
    (void)link;
}
void atalk_adsp_plug(adsp_link_t *link) {
    (void)link;
}
ppc_link_t *atalk_ppc_link_new(void) {
    return (ppc_link_t *)(void *)&g_link_dummy;
}
void atalk_ppc_link_free(ppc_link_t *link) {
    (void)link;
}
void atalk_ppc_plug(ppc_link_t *link) {
    (void)link;
}
aevt_link_t *atalk_aevt_link_new(void) {
    return (aevt_link_t *)(void *)&g_link_dummy;
}
void atalk_aevt_link_free(aevt_link_t *link) {
    (void)link;
}
void atalk_aevt_plug(aevt_link_t *link) {
    (void)link;
}

// ---- printer (appletalk_printer.c) ----------------------------------------
pap_printer_t *atalk_printer_register(void) {
    g_printer_registers++;
    return (pap_printer_t *)(void *)&g_link_dummy;
}
pap_link_t *atalk_printer_link_new(void) {
    return (pap_link_t *)(void *)&g_link_dummy;
}
void atalk_printer_link_free(pap_link_t *link) {
    (void)link;
}
void atalk_printer_register_timers(struct atalk_conn *conn, pap_link_t *link) {
    (void)conn;
    (void)link;
    g_printer_timer_registrations++;
}
void atalk_printer_plug(pap_link_t *link) {
    if (!link)
        g_printer_unplugs++;
}
void atalk_printer_link_down(void) {}
const char *atalk_printer_get_status(void) {
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

uint32_t atalk_printer_interpreter_jobs(void) {
    return 0;
}
uint32_t atalk_printer_interpreter_permanent_jobs(void) {
    return 0;
}
int atalk_printer_restart(char *err, size_t err_len) {
    (void)err, (void)err_len;
    return 0;
}

// ---- ADSP / PPC / AEVT -------------------------------------------------------
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
adsp_host_t *atalk_adsp_init(void) {
    return (adsp_host_t *)(void *)&g_link_dummy;
}
void atalk_adsp_install_objects(struct object *p) {
    (void)p;
}
void atalk_ppc_close_all(const char *r) {
    (void)r;
}
ppc_host_t *atalk_ppc_init(void) {
    return (ppc_host_t *)(void *)&g_link_dummy;
}
void atalk_ppc_install_objects(struct object *p) {
    (void)p;
}
aevt_host_t *atalk_aevt_init(void) {
    return (aevt_host_t *)(void *)&g_link_dummy;
}
void atalk_aevt_install_objects(struct object *p) {
    (void)p;
}

// ---- the network's configuration ---------------------------------------------
//
// Stateful, like the real modules, so a test can see that no machine's
// lifecycle touches it.

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
int atalk_afp_volume_add(const char *n, const char *p, char *e, size_t el) {
    for (int i = 0; i < 8; i++)
        if (g_stub_vols[i].in_use && strcmp(g_stub_vols[i].name, n) == 0) {
            snprintf(e, el, "taken");
            return -1;
        }
    for (int i = 0; i < 8; i++)
        if (!g_stub_vols[i].in_use) {
            g_stub_vols[i].in_use = true;
            snprintf(g_stub_vols[i].name, sizeof(g_stub_vols[i].name), "%s", n);
            snprintf(g_stub_vols[i].path, sizeof(g_stub_vols[i].path), "%s", p);
            g_stub_vols[i].vol_id = g_stub_next_vol_id++;
            return i;
        }
    snprintf(e, el, "full");
    return -1;
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
bool atalk_printer_get_enabled(void) {
    return g_printer_enabled;
}
const char *atalk_printer_get_name(void) {
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
bool atalk_printer_get_capture(void) {
    return g_printer_capture;
}
void atalk_printer_set_capture(bool en) {
    g_printer_capture = en;
}
