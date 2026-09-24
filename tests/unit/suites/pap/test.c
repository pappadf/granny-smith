// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The PAP server's capture: a job's PostScript is handed to the platform
// sink, whole, and never written anywhere by the core.  See Makefile.

#include "appletalk.h"
#include "appletalk_internal.h"
#include "laserwriter_job.h"
#include "test_assert.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// --- stubs: ATP, NBP, the clock ------------------------------------------------

void LOG_INDENT(int n) {
    (void)n;
}
// A scheduler to hand out, so the printer registers its timers; they run
// through the stubs below, never through it.
static int g_scheduler_dummy;
struct scheduler *atalk_scheduler(void) {
    return (struct scheduler *)&g_scheduler_dummy;
}
static uint64_t g_now_ns; // the guest clock, moved by the test
uint64_t atalk_now_ns(void) {
    return g_now_ns;
}

// Timers fire when the test runs them (fire_timers), those that are due.
#define MAX_ARMED 8
static struct {
    atalk_timer_t *t;
    uint64_t data;
    uint64_t due;
} g_armed[MAX_ARMED];
static int g_n_armed;
void atalk_timer_init(atalk_timer_t *t, const char *source_name, const char *event_name, atalk_timer_fn cb) {
    (void)source_name;
    (void)event_name;
    t->cb = cb;
}
void atalk_timer_arm(atalk_timer_t *t, uint64_t data, uint64_t delay_ns) {
    int i = 0;
    while (i < g_n_armed && !(g_armed[i].t == t && g_armed[i].data == data))
        i++;
    if (i == g_n_armed) {
        ASSERT_TRUE(g_n_armed < MAX_ARMED);
        g_n_armed++;
    }
    g_armed[i].t = t;
    g_armed[i].data = data;
    g_armed[i].due = g_now_ns + delay_ns;
}
void atalk_timer_cancel(atalk_timer_t *t, uint64_t data) {
    for (int i = 0; i < g_n_armed; i++)
        if (g_armed[i].t == t && g_armed[i].data == data)
            g_armed[i--] = g_armed[--g_n_armed];
}
void atalk_timer_cancel_all(atalk_timer_t *t) {
    for (int i = 0; i < g_n_armed; i++)
        if (g_armed[i].t == t)
            g_armed[i--] = g_armed[--g_n_armed];
}
static void fire_timers(void) {
    for (int i = 0; i < g_n_armed;) {
        if (g_armed[i].due > g_now_ns) {
            i++;
            continue;
        }
        atalk_timer_t *t = g_armed[i].t;
        uint64_t data = g_armed[i].data;
        g_armed[i] = g_armed[--g_n_armed];
        t->cb(t, data);
        i = 0; // the callback may have armed or cancelled others
    }
}

// Move the clock on by `ns` and run whatever fell due.
static void advance(uint64_t ns) {
    g_now_ns += ns;
    fire_timers();
}
void atp_unregister_socket_handler(uint8_t socket) {
    (void)socket;
}

static const atp_socket_handler_t *g_pap;
static void *g_pap_ctx;
int atp_register_socket_handler(uint8_t s, const atp_socket_handler_t *h, void *c) {
    (void)s;
    g_pap = h;
    g_pap_ctx = c;
    return 0;
}

// The last request the printer put out (its SendData), to answer.
static atp_request_callbacks_t g_req_cb;
static void *g_req_ctx;
static int g_req_handle;
static int g_close_requests;
atp_request_handle_t *atp_request_submit(const atp_request_params_t *p, const atp_request_callbacks_t *cb, void *ctx) {
    if (p->user[1] == PAP_FUNC_CLOSE)
        g_close_requests++;
    else {
        g_req_cb = *cb;
        g_req_ctx = ctx;
    }
    return (atp_request_handle_t *)&g_req_handle;
}
void atp_request_cancel(atp_request_handle_t *h) {
    (void)h;
}
// What the printer last answered a workstation's read with.
static char g_last_reply[256];
static void record_reply(const uint8_t *pl, int len) {
    int n = len < (int)sizeof g_last_reply - 1 ? len : (int)sizeof g_last_reply - 1;
    if (n < 0 || !pl)
        n = 0;
    memcpy(g_last_reply, pl ? pl : (const uint8_t *)"", (size_t)n);
    g_last_reply[n] = '\0';
}
int atp_responder_send_packets(const ddp_header_t *d, const atp_packet_t *a, const atp_response_packet_desc_t *p,
                               size_t n) {
    (void)d;
    (void)a;
    if (n)
        record_reply(p[0].payload, p[0].payload_len);
    return 0;
}
static int g_close_replies;
int atp_responder_send_simple(const ddp_header_t *d, const atp_packet_t *a, const uint8_t user[4], const uint8_t *pl,
                              int len, bool sts) {
    (void)d;
    (void)a;
    if (user[1] == PAP_FUNC_CLOSE_REPLY)
        g_close_replies++;
    else
        record_reply(pl, len);
    (void)sts;
    return 0;
}
static int g_nbp_entry;
static char g_nbp_name[40]; // what is advertised; "Taken" is refused
int atalk_nbp_publish(atalk_nbp_entry_t **e, const atalk_nbp_service_desc_t *d) {
    if (!e || !d || strcmp(d->object, "Taken") == 0)
        return -1;
    *e = (atalk_nbp_entry_t *)&g_nbp_entry;
    snprintf(g_nbp_name, sizeof g_nbp_name, "%s", d->object);
    return 0;
}
void atalk_nbp_withdraw(atalk_nbp_entry_t **e) {
    if (e)
        *e = NULL;
    g_nbp_name[0] = '\0';
}

// --- the platform's capture sink -------------------------------------------------

static int g_captures;
static uint8_t g_captured[256];
static size_t g_captured_len;
static bool g_captured_complete;
void laserwriter_sink_capture(const laserwriter_capture_t *cap) {
    g_captures++;
    g_captured_len = cap->ps_len < sizeof(g_captured) ? cap->ps_len : sizeof(g_captured);
    memcpy(g_captured, cap->ps, g_captured_len);
    g_captured_complete = cap->complete;
}

// --- driving a job -------------------------------------------------------------

// A PAP request from `node` on connection `conn`.
static void request(uint8_t node, uint8_t conn, uint8_t func, const uint8_t *data, int len) {
    ddp_header_t ddp = {0};
    ddp.llap.src = node;
    ddp.src_socket = 200;
    ddp.type = DDP_TYPE_ATP;
    atp_packet_t atp = {0};
    atp.user[0] = conn;
    atp.user[1] = func;
    atp.data = data;
    atp.data_len = len;
    atp.bitmap = 1;
    g_pap->handle_request(&ddp, &atp, g_pap_ctx);
}

static void open_conn(uint8_t conn) {
    uint8_t open_data[4] = {200, 8, 0, 0}; // workstation socket, flow quantum
    request(10, conn, PAP_FUNC_OPEN, open_data, 4);
    advance(20000000); // the printer's first SendData follows the OpenReply after a gap
}

// One SendData transaction answered with `n` fragments, the last with EOF.
static void answer(const char *const *parts, int n, bool eof) {
    ASSERT_TRUE(g_req_cb.on_response != NULL);
    for (int i = 0; i < n; i++) {
        atp_response_fragment_t f = {0};
        f.seq = (uint8_t)i;
        f.eom = (i == n - 1);
        f.user[2] = (eof && i == n - 1) ? 1 : 0;
        f.data = (const uint8_t *)parts[i];
        f.data_len = (int)strlen(parts[i]);
        g_req_cb.on_response(&f, g_req_ctx);
    }
    g_req_cb.on_complete((atp_request_handle_t *)&g_req_handle, ATP_REQUEST_RESULT_OK, g_req_ctx);
    advance(20000000);
}

static void setup(void) {
    g_captures = 0;
    g_captured_len = 0;
    g_close_requests = 0;
    g_close_replies = 0;
    memset(&g_req_cb, 0, sizeof(g_req_cb));
    atalk_printer_shutdown(); // the last test's connection goes
    g_n_armed = 0;
    atalk_printer_register();
    ASSERT_TRUE(g_pap != NULL);
}

// Three fragments of a job reach the sink as exactly those bytes, marked
// complete -- and nothing is written under /tmp, where the job used to be
// spooled (F-12).
TEST(a_job_reaches_the_capture_sink_whole) {
    setup();
    atalk_printer_stats_t before = *atalk_printer_get_stats();
    unlink("/tmp/laserwriter-job-00001.ps");
    open_conn(5);
    const char *parts[] = {"%!PS\n", "1 2 add pop\n", "showpage\n"};
    answer(parts, 3, true);
    ASSERT_EQ_INT(1, g_captures);
    ASSERT_EQ_INT(26, (int)g_captured_len);
    ASSERT_EQ_INT(0, memcmp(g_captured, "%!PS\n1 2 add pop\nshowpage\n", 26));
    ASSERT_TRUE(g_captured_complete);
    const atalk_printer_stats_t *after = atalk_printer_get_stats(); // F-33
    ASSERT_EQ_INT(1, (int)(after->jobs - before.jobs));
    ASSERT_EQ_INT(26, (int)(after->bytes - before.bytes));
    ASSERT_EQ_INT(1, (int)(after->captures - before.captures));
    ASSERT_EQ_INT(26, (int)after->last_capture);
    ASSERT_EQ_INT(0, (int)(after->aborts - before.aborts));
    struct stat st;
    ASSERT_TRUE(stat("/tmp/laserwriter-job-00001.ps", &st) != 0);
}

// A job past PRINTER_CAPTURE_MAX (64 here) is aborted, and none of it is
// handed over.
TEST(a_job_too_large_is_aborted) {
    setup();
    uint64_t aborts = atalk_printer_get_stats()->aborts;
    open_conn(6);
    const char *parts[] = {"0123456789012345678901234567890123456789", "0123456789012345678901234"}; // 65 bytes
    answer(parts, 2, false);
    ASSERT_EQ_INT(0, g_captures);
    ASSERT_EQ_INT(1, g_close_requests); // the workstation is told the job is over
    ASSERT_TRUE(strstr(atalk_printer_get_status(), "idle") != NULL);
    ASSERT_EQ_INT(1, (int)(atalk_printer_get_stats()->aborts - aborts));
}

// A CloseConn ends the job only when it is the session's: its connection id
// from the node that opened it.  Any id ended the active job (F-13).  A
// foreign one is still answered with a CloseReply.
TEST(a_foreign_closeconn_does_not_end_the_job) {
    setup();
    open_conn(5); // from node 10
    request(99, 77, PAP_FUNC_CLOSE, NULL, 0); // another id, another node
    request(99, 5, PAP_FUNC_CLOSE, NULL, 0); // the session's id, another node
    ASSERT_EQ_INT(2, g_close_replies);
    ASSERT_TRUE(strstr(atalk_printer_get_status(), "processing") != NULL);
    request(10, 5, PAP_FUNC_CLOSE, NULL, 0); // the session's own
    ASSERT_EQ_INT(3, g_close_replies);
    ASSERT_TRUE(strstr(atalk_printer_get_status(), "idle") != NULL);
}

// A workstation that goes quiet loses its connection after 120 s of guest
// time, whether or not anything else arrives.  The timeout was checked only
// when the next PAP packet came in, so a vanished client held the printer
// (N-22).
TEST(an_idle_connection_times_out_on_its_own) {
    setup();
    open_conn(7);
    advance(119ull * 1000000000ull);
    ASSERT_TRUE(strstr(atalk_printer_get_status(), "processing") != NULL);
    ASSERT_EQ_INT(0, g_close_requests);
    advance(2ull * 1000000000ull);
    ASSERT_TRUE(strstr(atalk_printer_get_status(), "idle") != NULL);
    ASSERT_EQ_INT(1, g_close_requests); // the workstation is told
}

// A printer rename that cannot be published leaves the printer named, and
// advertised, as it was; a name longer than NBP allows is refused, not cut
// short (N-23: the name was stored first, and enable truncated what set_name
// rejects).
TEST(a_printer_rename_that_cannot_be_published_changes_nothing) {
    setup();
    char err[128];
    ASSERT_EQ_INT(0, atalk_printer_set_name("Before", err, sizeof err));
    ASSERT_EQ_INT(0, strcmp(g_nbp_name, "Before"));
    ASSERT_TRUE(atalk_printer_set_name("Taken", err, sizeof err) != 0);
    ASSERT_EQ_INT(0, strcmp(atalk_printer_get_name(), "Before"));
    ASSERT_EQ_INT(0, strcmp(g_nbp_name, "Before"));
    ASSERT_TRUE(atalk_printer_enable("A name of forty characters, past NBP's 32") != 0);
    ASSERT_EQ_INT(0, strcmp(atalk_printer_get_name(), "Before"));
}

// A query's "= flush" marker split across two fragments is still found, and
// answered on the workstation's next read with the PatchPrep state ("0": not
// installed).
TEST(a_query_split_across_fragments_is_answered) {
    setup();
    open_conn(8);
    const char *parts[] = {"%!PS\n/PatchPrep where { pop 1 } { 0 } ifelse = fl", "ush\n"};
    answer(parts, 2, false);
    g_last_reply[0] = '\0';
    request(10, 8, PAP_FUNC_SENDDATA, NULL, 0); // the workstation reads
    ASSERT_TRUE(g_last_reply[0] == '0');
}

int main(void) {
    RUN(a_job_reaches_the_capture_sink_whole);
    RUN(a_job_too_large_is_aborted);
    RUN(a_foreign_closeconn_does_not_end_the_job);
    RUN(an_idle_connection_times_out_on_its_own);
    RUN(a_printer_rename_that_cannot_be_published_changes_nothing);
    RUN(a_query_split_across_fragments_is_answered);
    printf("pap: all tests passed\n");
    return 0;
}
