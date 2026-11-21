// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The ImageWriter's PAP server (appletalk_imagewriter.c) on the wire: what it
// answers an OpenConn with, the CloseConn it sends, and the name it will
// advertise.  ATP, NBP, the clock and the printer are stubbed here.

#include "appletalk.h"
#include "appletalk_internal.h"
#include "iw_printer.h"
#include "macroman.h"
#include "test_assert.h"

#include <stdio.h>
#include <string.h>

// --- stubs: the clock and timers ------------------------------------------------

static uint64_t g_now_ns;
uint64_t atalk_now_ns(void) {
    return g_now_ns;
}
void atalk_timer_init(struct atalk_conn *conn, atalk_timer_t *t, const char *source_name, const char *event_name,
                      atalk_timer_fn cb) {
    (void)conn;
    (void)source_name;
    (void)event_name;
    t->cb = cb;
}
void atalk_timer_arm(atalk_timer_t *t, uint64_t data, uint64_t delay_ns) {
    (void)t;
    (void)data;
    (void)delay_ns;
}
void atalk_timer_cancel_all(atalk_timer_t *t) {
    (void)t;
}

// --- stubs: ATP -----------------------------------------------------------------

static const atp_socket_handler_t *g_handler;
int atp_register_socket_handler(uint8_t s, const atp_socket_handler_t *h, void *c) {
    (void)s;
    (void)c;
    g_handler = h;
    return 0;
}

// The CloseConn the server last asked for, and how many.
static int g_close_requests;
static atp_request_params_t g_close_params;
static int g_req_handle;
atp_request_handle_t *atp_request_submit(const atp_request_params_t *p, const atp_request_callbacks_t *cb, void *ctx) {
    (void)cb;
    (void)ctx;
    if (p->user[1] == PAP_FUNC_CLOSE) {
        g_close_requests++;
        g_close_params = *p;
    }
    return (atp_request_handle_t *)&g_req_handle;
}
void atp_request_cancel(atp_request_handle_t *h) {
    (void)h;
}

// Every response the server sent, and the last one's result word.
static int g_responses;
static uint16_t g_last_result;
static uint8_t g_last_func;
int atp_responder_send_simple(const ddp_header_t *d, const atp_packet_t *a, const uint8_t user[4], const uint8_t *pl,
                              int len, bool sts) {
    (void)d;
    (void)a;
    (void)sts;
    g_responses++;
    g_last_func = user[1];
    g_last_result = (pl && len >= 4) ? (uint16_t)((pl[2] << 8) | pl[3]) : 0;
    return 0;
}
int atp_responder_send_packets(const ddp_header_t *d, const atp_packet_t *a, const atp_response_packet_desc_t *p,
                               size_t n) {
    (void)d;
    (void)a;
    (void)p;
    (void)n;
    g_responses++;
    return 0;
}
// --- stubs: NBP -----------------------------------------------------------------

// appletalk.c's rule: required, MacRoman-encodable, at most 32 MacRoman bytes.
int atalk_nbp_name_check(const char *what, const char *name, char *err, size_t err_len) {
    uint8_t mac[ATALK_NBP_NAME_MAX * 4];
    int len = name && *name ? macroman_from_utf8(name, mac, sizeof(mac)) : 0;
    if (len <= 0 || len > ATALK_NBP_NAME_MAX) {
        snprintf(err, err_len, "%s '%s' refused", what, name ? name : "");
        return -1;
    }
    return 0;
}
static int g_nbp_entry;
static char g_nbp_name[ATALK_NBP_TEXT_CAP];
int atalk_nbp_publish(atalk_nbp_entry_t **e, const atalk_nbp_service_desc_t *d) {
    if (strcmp(d->object, "Taken") == 0)
        return -1;
    *e = (atalk_nbp_entry_t *)&g_nbp_entry;
    snprintf(g_nbp_name, sizeof g_nbp_name, "%s", d->object);
    return 0;
}
void atalk_nbp_withdraw(atalk_nbp_entry_t **e) {
    *e = NULL;
    g_nbp_name[0] = '\0';
}
static int g_network;
atalk_network_t *appletalk_network(void) {
    return (atalk_network_t *)&g_network;
}

// --- stubs: the printer ---------------------------------------------------------

static int g_printer;
static int g_jobs_ended;
iw_printer_t *iw_printer_localtalk(void) {
    return (iw_printer_t *)&g_printer;
}
bool iw_printer_paper_out(const iw_printer_t *p) {
    (void)p;
    return false;
}
bool iw_printer_selected(const iw_printer_t *p) {
    (void)p;
    return true;
}
bool iw_printer_sheet_feeder(const iw_printer_t *p) {
    (void)p;
    return false;
}
bool iw_printer_busy(const iw_printer_t *p) {
    (void)p;
    return false;
}
void iw_printer_feed(iw_printer_t *p, const uint8_t *data, size_t len) {
    (void)p;
    (void)data;
    (void)len;
}
void iw_printer_end_network_job(iw_printer_t *p, bool abandoned) {
    (void)p;
    (void)abandoned;
    g_jobs_ended++;
}

// --- driving the server ---------------------------------------------------------

// A PAP request from node 10 on connection `conn`.
static void request(uint8_t conn, uint8_t func, const uint8_t *data, int len) {
    ddp_header_t ddp = {0};
    ddp.llap.src = 10;
    ddp.src_socket = 200;
    ddp.type = DDP_TYPE_ATP;
    atp_packet_t atp = {0};
    atp.user[0] = conn;
    atp.user[1] = func;
    atp.data = data;
    atp.data_len = len;
    atp.bitmap = 1;
    atp.ctl = 0x40 | 0x20; // TReq, XO
    g_handler->handle_request(&ddp, &atp, NULL);
}

static void open_conn(uint8_t conn) {
    uint8_t open_data[4] = {200, 8, 0, 0}; // workstation socket, flow quantum, wait time
    request(conn, PAP_FUNC_OPEN, open_data, 4);
}

static void setup(void) {
    atalk_imagewriter_publish(false);
    atalk_imagewriter_unplug();
    atalk_imagewriter_register();
    g_responses = 0;
    g_close_requests = 0;
    g_last_result = 0;
    g_last_func = 0;
    ASSERT_TRUE(g_handler != NULL);
}

// Published, a well-formed OpenConn is accepted.
TEST(an_open_is_accepted_while_published) {
    setup();
    ASSERT_EQ_INT(0, atalk_imagewriter_publish(true));
    open_conn(3);
    ASSERT_EQ_INT(1, g_responses);
    ASSERT_EQ_INT(PAP_FUNC_OPEN_REPLY, g_last_func);
    ASSERT_EQ_INT(PAP_RESULT_OK, g_last_result);
}

// A withdrawn printer stays silent, as the LaserWriter does: PrinterBusy makes
// the workstation retry without end, silence lets its open time out.
TEST(a_withdrawn_printer_does_not_answer_busy) {
    setup();
    open_conn(4);
    ASSERT_EQ_INT(0, g_responses);
}

// A malformed OpenConn (its 4 bytes of data short) is dropped, not answered.
TEST(a_short_open_is_dropped) {
    setup();
    ASSERT_EQ_INT(0, atalk_imagewriter_publish(true));
    uint8_t short_data[2] = {200, 8};
    request(5, PAP_FUNC_OPEN, short_data, 2);
    ASSERT_EQ_INT(0, g_responses);
}

// A second open while a connection is up is answered busy.
TEST(a_second_open_is_busy) {
    setup();
    ASSERT_EQ_INT(0, atalk_imagewriter_publish(true));
    open_conn(6);
    open_conn(7);
    ASSERT_EQ_INT(2, g_responses);
    ASSERT_EQ_INT(PAP_RESULT_BUSY, g_last_result);
}

// The CloseConn the server sends on withdrawal is the LaserWriter's: XO with
// SendData's release hint, retried five times.
TEST(the_closeconn_matches_the_laserwriters) {
    setup();
    ASSERT_EQ_INT(0, atalk_imagewriter_publish(true));
    open_conn(8);
    ASSERT_EQ_INT(0, atalk_imagewriter_publish(false));
    ASSERT_EQ_INT(1, g_close_requests);
    ASSERT_EQ_INT(ATP_TRANSACTION_XO, g_close_params.mode);
    ASSERT_EQ_INT(2, g_close_params.trel_timer_hint);
    ASSERT_EQ_INT(5, g_close_params.retry_limit);
    ASSERT_EQ_INT(8, g_close_params.user[0]);
}

// The name rule is NBP's: MacRoman-encodable and at most 32 MacRoman bytes,
// not 32 UTF-8 bytes.
TEST(the_name_rule_is_macroman) {
    setup();
    char err[160] = "";
    // 20 accented letters: 40 bytes of UTF-8, 20 of MacRoman.
    char accented[64] = "";
    for (int i = 0; i < 20; i++)
        strcat(accented, "\xC3\xA9");
    ASSERT_EQ_INT(0, atalk_imagewriter_set_name(accented, err, sizeof err));
    ASSERT_EQ_INT(0, strcmp(atalk_imagewriter_name(), accented));
    // A character MacRoman lacks is refused, with NBP's reason.
    ASSERT_EQ_INT(-1, atalk_imagewriter_set_name("\xE5\x90\x8D", err, sizeof err));
    ASSERT_TRUE(strstr(err, "refused") != NULL);
    ASSERT_EQ_INT(0, strcmp(atalk_imagewriter_name(), accented));
    // A name another entity holds leaves the advertised name as it was.
    ASSERT_EQ_INT(0, atalk_imagewriter_set_name("Office", err, sizeof err));
    ASSERT_EQ_INT(0, atalk_imagewriter_publish(true));
    ASSERT_EQ_INT(-1, atalk_imagewriter_set_name("Taken", err, sizeof err));
    ASSERT_TRUE(strstr(err, "already taken") != NULL);
    ASSERT_EQ_INT(0, strcmp(atalk_imagewriter_name(), "Office"));
    ASSERT_EQ_INT(0, strcmp(g_nbp_name, "Office"));
}

int main(void) {
    RUN(an_open_is_accepted_while_published);
    RUN(a_withdrawn_printer_does_not_answer_busy);
    RUN(a_short_open_is_dropped);
    RUN(a_second_open_is_busy);
    RUN(the_closeconn_matches_the_laserwriters);
    RUN(the_name_rule_is_macroman);
    fprintf(stderr, "All pap_imagewriter tests passed.\n");
    return 0;
}
