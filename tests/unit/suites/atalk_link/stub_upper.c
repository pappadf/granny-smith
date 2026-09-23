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
#include "appletalk_internal.h"
#include "appletalk_ppc.h"

#include <string.h>

void LOG_INDENT(int n) {
    (void)n;
}

// ---- AFP server (appletalk_server.c) --------------------------------------
int g_afp_calls;
uint8_t g_afp_last_opcode;
uint16_t g_afp_last_session;

uint32_t afp_handle_command(uint16_t session_id, uint8_t opcode, const uint8_t *in, int in_len, uint8_t *out,
                            int out_max, int *out_len) {
    (void)in, (void)in_len, (void)out, (void)out_max;
    g_afp_calls++;
    g_afp_last_opcode = opcode;
    g_afp_last_session = session_id;
    if (out_len)
        *out_len = 0;
    return 0;
}
void afp_session_closed(uint16_t s) {
    (void)s;
}
void afp_reset_transient_state(void) {}
uint32_t afp_session_open_forks(uint16_t s) {
    (void)s;
    return 0;
}
void atalk_server_init(void) {}
void atalk_server_delete(void) {}
int atalk_build_status_block(const char *a, const char *b, uint8_t **o, size_t *l) {
    (void)a, (void)b;
    *o = NULL;
    *l = 0;
    return -1;
}
const char *atalk_afp_get_name(void) {
    return "Test Server";
}
int atalk_afp_set_name(const char *n, char *e, size_t el) {
    (void)n, (void)e, (void)el;
    return 0;
}
bool atalk_afp_get_enabled(void) {
    return true;
}
int atalk_afp_set_enabled(bool en, char *e, size_t el) {
    (void)en, (void)e, (void)el;
    return 0;
}
const char *atalk_afp_get_message(void) {
    return "";
}
int atalk_afp_set_message(const char *m, char *e, size_t el) {
    (void)m, (void)e, (void)el;
    return 0;
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
int atalk_afp_volume_add(const char *n, const char *p, char *e, size_t el) {
    (void)n, (void)p, (void)e, (void)el;
    return -1;
}
int atalk_afp_volume_remove(const char *n, char *e, size_t el) {
    (void)n, (void)e, (void)el;
    return -1;
}
int atalk_afp_volume_max(void) {
    return 0;
}
int atalk_afp_volume_find(const char *n) {
    (void)n;
    return -1;
}
bool atalk_afp_volume_in_use(int s) {
    (void)s;
    return false;
}
const char *atalk_afp_volume_name(int s) {
    (void)s;
    return "";
}
const char *atalk_afp_volume_path(int s) {
    (void)s;
    return "";
}
unsigned atalk_afp_volume_vol_id(int s) {
    (void)s;
    return 0;
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
bool atalk_printer_is_enabled(void) {
    return false;
}
const char *atalk_printer_object_name(void) {
    return "";
}
int atalk_printer_set_enabled(bool en, char *e, size_t el) {
    (void)en, (void)e, (void)el;
    return 0;
}
int atalk_printer_set_name(const char *n, char *e, size_t el) {
    (void)n, (void)e, (void)el;
    return 0;
}
const char *atalk_printer_status_text(void) {
    return "";
}
bool atalk_printer_has_interpreter(void) {
    return false;
}
bool atalk_printer_capture_get(void) {
    return false;
}
void atalk_printer_capture_set(bool en) {
    (void)en;
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
