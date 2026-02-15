// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_printer.c
// Emulated LaserWriter printer service over AppleTalk PAP protocol.

#include "appletalk.h"
#include "appletalk_internal.h"
#include "log.h"
#include "platform.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("appletalk");

#define PRINTER_STATUS_MAX          255
#define PRINTER_OBJECT_MAX          32
#define PRINTER_SPOOL_PATH_MAX      160
#define PRINTER_DEFAULT_OBJECT      "LaserWriter (Sim)"
#define PRINTER_STATUS_IDLE         "status: idle"
#define PRINTER_STATUS_BUSY         "status: print spooler processing job"
#define PRINTER_ENTITY_TYPE         "LaserWriter"
#define PRINTER_SPOOL_DIR           "/tmp"
#define PAP_SENDDATA_RETRY_MS       8000u
#define PAP_SENDDATA_RETRY_LIMIT    12
#define PAP_SESSION_TIMEOUT_MS      120000u
#define PAP_QUERY_PLACEHOLDER_LIMIT 8

// Kinds of PostScript queries we synthesize replies for.
typedef enum { PAP_QUERY_NONE = 0, PAP_QUERY_PATCH_STATUS, PAP_QUERY_FONT_LIST } pap_query_mode_t;

// Structure capturing global printer service state.
typedef struct {
    bool initialized;
    bool enabled;
    char object_name[PRINTER_OBJECT_MAX + 1];
    char status_text[PRINTER_STATUS_MAX + 1];
    uint8_t status_len;
    atalk_nbp_entry_t *nbp_entry;
    uint32_t job_counter;
    bool patch_installed; // True once the PatchPrep procset has been uploaded
} pap_printer_service_t;

// Structure tracking a held PAP SendData request while we wait for data to emit.
typedef struct {
    bool in_use;
    ddp_header_t ddp;
    atp_packet_t atp;
} pap_status_credit_t;

// Structure representing the single active PAP session (LaserWriter handles one job at a time).
typedef struct {
    bool active;
    uint8_t conn_id;
    uint8_t client_socket;
    uint8_t client_flow_quantum;
    uint8_t server_flow_quantum;
    uint16_t next_send_seq;
    uint16_t inflight_seq;
    bool awaiting_data;
    bool eof_pending;
    bool blocked_for_reply;
    atalk_socket_addr_t client_addr;
    atp_request_handle_t *send_handle;
    FILE *spool;
    char spool_path[PRINTER_SPOOL_PATH_MAX];
    uint32_t job_id;
    size_t bytes_received;
    uint64_t last_activity_ms;
    char pending_reply[PRINTER_STATUS_MAX + 1];
    uint8_t pending_reply_len;
    bool pending_reply_ready;
    char query_window[16];
    uint8_t query_window_len;
    char patch_window[32];
    uint8_t patch_window_len;
    bool query_detected;
    bool query_waiting_job;
    uint8_t query_placeholder_eofs;
    bool expecting_patch; // Query requested a PatchPrep upload before the job
    bool patch_active; // Currently ingesting a PatchPrep payload
    pap_query_mode_t query_mode;
    char font_query_window[32];
    uint8_t font_query_window_len;
    bool font_query_detected;
    pap_status_credit_t pending_status_reads[PAP_MAX_FLOW_QUANTUM];
    uint8_t pending_status_head;
    uint8_t pending_status_count;
} pap_session_t;

typedef struct {
    bool active;
    uint8_t conn_id;
    char text[PRINTER_STATUS_MAX + 1];
    bool payload_sent;
    atalk_socket_addr_t client_addr;
} pap_completion_stream_t;

// Context for CloseConn ATP requests so we can log completion status.
typedef struct {
    uint8_t conn_id;
} pap_close_request_ctx_t;

static pap_printer_service_t g_printer = {.initialized = false,
                                          .enabled = false,
                                          .object_name = PRINTER_DEFAULT_OBJECT,
                                          .status_text = PRINTER_STATUS_IDLE,
                                          .status_len = (uint8_t)(sizeof(PRINTER_STATUS_IDLE) - 1),
                                          .nbp_entry = NULL,
                                          .job_counter = 0,
                                          .patch_installed = false};
static pap_session_t g_session;
static pap_completion_stream_t g_completion;

// Forward declarations for helper routines.
static void pap_printer_init(void);
static uint64_t pap_now_ms(void);
static void pap_session_reset(void);
static void pap_printer_set_status_fmt(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;
static void pap_printer_set_status_idle(void);
static void pap_printer_set_status_disabled(void);
static int pap_build_status_payload(uint8_t socket_id, uint8_t flow_quantum, uint16_t result_code, uint8_t *out,
                                    size_t out_max);
static void pap_session_finish(bool success, const char *reason, bool notify_client);
static void pap_session_abort(const char *reason);
static bool pap_session_open_spool(pap_session_t *sess);
static void pap_session_rewind_spool(pap_session_t *sess);
static void pap_update_progress_status(void);
static void pap_session_record_activity(void);
static void pap_session_check_timeout(void);
static void pap_queue_postscript_reply(const char *text);
static bool pap_detect_flush_sequence(pap_session_t *sess, const uint8_t *data, int len);
static bool pap_detect_patchprep_sequence(pap_session_t *sess, const uint8_t *data, int len);
static bool pap_detect_fontlist_sequence(pap_session_t *sess, const uint8_t *data, int len);
static bool pap_consume_query_eof(pap_session_t *sess);
static void pap_handle_patch_complete(pap_session_t *sess);
static bool pap_finalize_job(const char *reason);
static void pap_queue_font_list_reply(void);
static bool pap_fragment_is_placeholder_eof(const atp_response_fragment_t *fragment);
static void pap_status_queue_reset(void);
static bool pap_status_queue_enqueue(const ddp_header_t *ddp, const atp_packet_t *atp);
static pap_status_credit_t *pap_status_queue_head(void);
static void pap_status_queue_pop(void);
static void pap_status_queue_drain(const char *text, bool eof_on_last);
static bool pap_try_deliver_pending_reply(void);
static int pap_format_status_line(const char *text, char *out, size_t out_len);
static void pap_completion_set(uint8_t conn_id, const char *text, const atalk_socket_addr_t *addr);
static void pap_completion_clear(void);
static void pap_close_request_complete(atp_request_handle_t *handle, atp_request_result_t result, void *ctx);
static void pap_issue_close_conn(uint8_t conn_id, const atalk_socket_addr_t *addr);
static void pap_log_session_state(const char *tag);
static void pap_send_data_response(const ddp_header_t *ddp, atp_packet_t *atp, uint8_t conn_id, const uint8_t *payload,
                                   int len, bool set_eof);
static bool pap_issue_senddata_request(void);
static void pap_handle_data_fragment(const atp_response_fragment_t *fragment, void *ctx);
static void pap_handle_data_complete(atp_request_handle_t *handle, atp_request_result_t result, void *ctx);
static void pap_handle_open(const ddp_header_t *ddp, atp_packet_t *atp);
static void pap_handle_send_status(const ddp_header_t *ddp, atp_packet_t *atp);
static void pap_handle_close_conn(const ddp_header_t *ddp, atp_packet_t *atp);
static void pap_handle_status_read(const ddp_header_t *ddp, atp_packet_t *atp);
static void pap_handle_tickle(void);
static void pap_socket_request_handler(const ddp_header_t *ddp, atp_packet_t *request, void *ctx);
static const char *pap_func_name(uint8_t func);
static void pap_format_request_detail(char *dst, size_t dst_len, uint8_t func, const atp_packet_t *packet);

// Helper returning the current platform tick count in milliseconds.
static uint64_t pap_now_ms(void) {
    return platform_ticks();
}

// Ensures the printer service is initialized exactly once.
static void pap_printer_init(void) {
    if (g_printer.initialized)
        return;
    memset(&g_session, 0, sizeof(g_session));
    g_session.server_flow_quantum = PAP_MAX_FLOW_QUANTUM;
    g_session.next_send_seq = 1;
    strncpy(g_printer.object_name, PRINTER_DEFAULT_OBJECT, sizeof(g_printer.object_name) - 1);
    strncpy(g_printer.status_text, PRINTER_STATUS_IDLE, sizeof(g_printer.status_text) - 1);
    g_printer.status_text[sizeof(g_printer.status_text) - 1] = '\0';
    g_printer.status_len = (uint8_t)strlen(g_printer.status_text);
    g_printer.initialized = true;
}

// Resets the active session, canceling pending ATP requests and closing the spool file.
static void pap_session_reset(void) {
    if (g_session.send_handle) {
        atp_request_cancel(g_session.send_handle);
        g_session.send_handle = NULL;
    }
    if (g_session.spool) {
        fclose(g_session.spool);
        g_session.spool = NULL;
    }
    memset(g_session.spool_path, 0, sizeof(g_session.spool_path));
    g_session.active = false;
    g_session.awaiting_data = false;
    g_session.eof_pending = false;
    g_session.blocked_for_reply = false;
    g_session.conn_id = 0;
    g_session.client_socket = 0;
    g_session.client_flow_quantum = 0;
    g_session.server_flow_quantum = PAP_MAX_FLOW_QUANTUM;
    g_session.next_send_seq = 1;
    g_session.inflight_seq = 0;
    g_session.client_addr.net = 0;
    g_session.client_addr.node = 0;
    g_session.client_addr.socket = 0;
    g_session.job_id = 0;
    g_session.bytes_received = 0;
    g_session.last_activity_ms = 0;
    g_session.pending_reply[0] = '\0';
    g_session.pending_reply_len = 0;
    g_session.pending_reply_ready = false;
    g_session.query_detected = false;
    g_session.query_waiting_job = false;
    g_session.query_placeholder_eofs = 0;
    g_session.expecting_patch = false;
    g_session.patch_active = false;
    g_session.query_mode = PAP_QUERY_NONE;
    g_session.font_query_window_len = 0;
    g_session.font_query_detected = false;
    pap_status_queue_reset();
    g_session.query_window_len = 0;
    g_session.patch_window_len = 0;
}

// Updates the status string with formatted text (truncated to the PAP maximum).
static void pap_printer_set_status_fmt(const char *fmt, ...) {
    char buffer[PRINTER_STATUS_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    if (written < 0)
        return;
    if (written > PRINTER_STATUS_MAX)
        written = PRINTER_STATUS_MAX;
    memcpy(g_printer.status_text, buffer, (size_t)written);
    g_printer.status_text[written] = '\0';
    g_printer.status_len = (uint8_t)written;
}

// Sets the status string to the default idle message.
static void pap_printer_set_status_idle(void) {
    pap_printer_set_status_fmt("%s", PRINTER_STATUS_IDLE);
}

// Sets the status string to the disabled/offline message.
static void pap_printer_set_status_disabled(void) {
    pap_printer_set_status_fmt("%s", PRINTER_STATUS_IDLE);
}

// Builds the OpenConn/Status reply payload per PAP spec.
static int pap_build_status_payload(uint8_t socket_id, uint8_t flow_quantum, uint16_t result_code, uint8_t *out,
                                    size_t out_max) {
    if (!out || out_max < (size_t)(5 + g_printer.status_len))
        return -1;
    out[0] = socket_id;
    out[1] = flow_quantum;
    out[2] = (uint8_t)(result_code >> 8);
    out[3] = (uint8_t)(result_code & 0xFF);
    out[4] = g_printer.status_len;
    if (g_printer.status_len > 0)
        memcpy(&out[5], g_printer.status_text, g_printer.status_len);
    return (int)(5 + g_printer.status_len);
}

// Records recent activity to keep the connection timer alive.
static void pap_session_record_activity(void) {
    if (!g_session.active)
        return;
    g_session.last_activity_ms = pap_now_ms();
}

// Terminates the active session, optionally notifying the workstation beforehand.
static void pap_session_finish(bool success, const char *reason, bool notify_client) {
    if (!g_session.active) {
        pap_session_reset();
        return;
    }

    pap_log_session_state(success ? "finish-success" : "finish-abort");

    uint32_t job_id = g_session.job_id;
    uint8_t closing_conn = g_session.conn_id;
    atalk_socket_addr_t closing_addr = g_session.client_addr;
    size_t bytes = g_session.bytes_received;
    char saved_path[PRINTER_SPOOL_PATH_MAX];
    saved_path[0] = '\0';
    if (g_session.spool_path[0])
        strncpy(saved_path, g_session.spool_path, sizeof(saved_path) - 1);
    saved_path[sizeof(saved_path) - 1] = '\0';

    LOG(success ? 2 : 1, "pap: job %u %s (%s)", job_id, success ? "complete" : "aborted", reason ? reason : "done");

    const char *final_msg = PRINTER_STATUS_IDLE;

    if (notify_client && closing_conn && closing_addr.socket != 0)
        pap_issue_close_conn(closing_conn, &closing_addr);

    pap_status_queue_drain(final_msg, true);

    if (closing_conn)
        pap_completion_set(closing_conn, final_msg, &g_session.client_addr);

    pap_session_reset();
    if (!g_printer.enabled) {
        pap_printer_set_status_disabled();
        return;
    }
    pap_printer_set_status_fmt("%s", final_msg);
}

// Convenience helper to abort the session with an error message.
static void pap_session_abort(const char *reason) {
    pap_session_finish(false, reason, true);
}

// Opens (or creates) a spool file for the current job.
static bool pap_session_open_spool(pap_session_t *sess) {
    if (!sess)
        return false;
    snprintf(sess->spool_path, sizeof(sess->spool_path), PRINTER_SPOOL_DIR "/laserwriter-job-%05u.ps", sess->job_id);
    sess->spool = fopen(sess->spool_path, "wb");
    if (!sess->spool) {
        LOG(1, "pap: unable to open spool '%s': %s", sess->spool_path, strerror(errno));
        sess->spool_path[0] = '\0';
        return false;
    }
    LOG(2, "pap: capturing job %u in %s", sess->job_id, sess->spool_path);
    return true;
}

// Truncates the current spool file so the real job can overwrite the query payload.
static void pap_session_rewind_spool(pap_session_t *sess) {
    if (!sess)
        return;
    if (!sess->spool_path[0])
        return;
    if (sess->spool) {
        fclose(sess->spool);
        sess->spool = NULL;
    }
    sess->spool = fopen(sess->spool_path, "wb");
    if (!sess->spool)
        LOG(1, "pap: unable to rewind spool '%s': %s", sess->spool_path, strerror(errno));
    else
        LOG(4, "pap: rewound spool for job %u", (unsigned)sess->job_id);
}

// Updates the status string with the current job progress.
static void pap_update_progress_status(void) {
    if (!g_session.active)
        return;
    pap_printer_set_status_fmt("%s", PRINTER_STATUS_BUSY);
}

// Aborts connections that exceeded the PAP inactivity timer.
static void pap_session_check_timeout(void) {
    if (!g_session.active)
        return;
    uint64_t now = pap_now_ms();
    if (g_session.last_activity_ms == 0)
        g_session.last_activity_ms = now;
    if (now - g_session.last_activity_ms >= PAP_SESSION_TIMEOUT_MS) {
        LOG(1, "pap: connection timeout for job %u", g_session.job_id);
        pap_session_abort("connection timeout");
    }
}

// Queues a short reply string (e.g., PostScript '=' output) for delivery over the status channel.
static void pap_queue_postscript_reply(const char *text) {
    if (!text || !g_session.active)
        return;
    size_t len = strlen(text);
    if (len > PRINTER_STATUS_MAX)
        len = PRINTER_STATUS_MAX;
    memcpy(g_session.pending_reply, text, len);
    g_session.pending_reply[len] = '\0';
    g_session.pending_reply_len = (uint8_t)len;
    g_session.pending_reply_ready = true;
    g_session.blocked_for_reply = true;
    LOG(2, "pap: queued synthetic PostScript reply '%s'", g_session.pending_reply);
    pap_log_session_state("queue-reply");
    pap_try_deliver_pending_reply();
}

// Queues a newline-delimited list of built-in fonts for font queries.
static void pap_queue_font_list_reply(void) {
    static const char *k_font_names[] = {"Courier",
                                         "Courier-Bold",
                                         "Courier-Oblique",
                                         "Courier-BoldOblique",
                                         "Helvetica",
                                         "Helvetica-Bold",
                                         "Helvetica-Oblique",
                                         "Helvetica-BoldOblique",
                                         "Times-Roman",
                                         "Times-Bold",
                                         "Times-Italic",
                                         "Times-BoldItalic",
                                         "Symbol",
                                         "*"};
    char buffer[PRINTER_STATUS_MAX + 1];
    size_t offset = 0;
    size_t total = sizeof(k_font_names) / sizeof(k_font_names[0]);
    for (size_t i = 0; i < total; i++) {
        const char *name = k_font_names[i];
        size_t name_len = strlen(name);
        if (name_len == 0)
            continue;
        if (offset + name_len >= sizeof(buffer))
            break;
        memcpy(&buffer[offset], name, name_len);
        offset += name_len;
        if (i + 1 < total && offset + 1 < sizeof(buffer))
            buffer[offset++] = '\n';
    }
    if (offset >= sizeof(buffer))
        offset = sizeof(buffer) - 1;
    buffer[offset] = '\0';
    LOG(2, "pap: queued synthetic font list reply (%zu bytes)", offset);
    pap_queue_postscript_reply(buffer);
}

// Lightweight detector for the literal sequence "= flush" spanning fragments.
static bool pap_detect_flush_sequence(pap_session_t *sess, const uint8_t *data, int len) {
    static const char pattern[] = "= flush";
    const int pattern_len = (int)(sizeof(pattern) - 1);
    if (!sess || !data || len <= 0)
        return false;
    for (int i = 0; i < len; i++) {
        if (sess->query_window_len < sizeof(sess->query_window)) {
            sess->query_window[sess->query_window_len++] = (char)data[i];
        } else {
            memmove(sess->query_window, sess->query_window + 1, sizeof(sess->query_window) - 1);
            sess->query_window[sizeof(sess->query_window) - 1] = (char)data[i];
        }
        if (sess->query_window_len >= pattern_len) {
            if (memcmp(&sess->query_window[sess->query_window_len - pattern_len], pattern, pattern_len) == 0)
                return true;
        }
    }
    return false;
}

// Lightweight detector for the "PatchPrep" token so we know when the patch payload begins.
static bool pap_detect_patchprep_sequence(pap_session_t *sess, const uint8_t *data, int len) {
    static const char pattern[] = "PatchPrep";
    const int pattern_len = (int)(sizeof(pattern) - 1);
    if (!sess || !data || len <= 0)
        return false;
    for (int i = 0; i < len; i++) {
        if (sess->patch_window_len < sizeof(sess->patch_window)) {
            sess->patch_window[sess->patch_window_len++] = (char)data[i];
        } else {
            memmove(sess->patch_window, sess->patch_window + 1, sizeof(sess->patch_window) - 1);
            sess->patch_window[sizeof(sess->patch_window) - 1] = (char)data[i];
        }
        if (sess->patch_window_len >= pattern_len) {
            if (memcmp(&sess->patch_window[sess->patch_window_len - pattern_len], pattern, pattern_len) == 0)
                return true;
        }
    }
    return false;
}

// Lightweight detector for the font list query PostScript sequence.
static bool pap_detect_fontlist_sequence(pap_session_t *sess, const uint8_t *data, int len) {
    static const char pattern[] = "%%?BeginFontListQuery";
    const int pattern_len = (int)(sizeof(pattern) - 1);
    if (!sess || !data || len <= 0)
        return false;
    for (int i = 0; i < len; i++) {
        if (sess->font_query_window_len < sizeof(sess->font_query_window)) {
            sess->font_query_window[sess->font_query_window_len++] = (char)data[i];
        } else {
            memmove(sess->font_query_window, sess->font_query_window + 1, sizeof(sess->font_query_window) - 1);
            sess->font_query_window[sizeof(sess->font_query_window) - 1] = (char)data[i];
        }
        if (sess->font_query_window_len >= pattern_len) {
            size_t start = sess->font_query_window_len - pattern_len;
            if (memcmp(&sess->font_query_window[start], pattern, pattern_len) == 0)
                return true;
        }
    }
    return false;
}

// Consumes the EOF from the query handshake so the workstation can submit the real job.
static bool pap_consume_query_eof(pap_session_t *sess) {
    if (!sess || !sess->query_waiting_job)
        return false;
    LOG(2, "pap: query handshake complete, awaiting real job");
    sess->query_detected = false;
    sess->query_waiting_job = false;
    sess->query_placeholder_eofs = 0;
    sess->eof_pending = false;
    sess->bytes_received = 0;
    sess->query_mode = PAP_QUERY_NONE;
    sess->font_query_window_len = 0;
    sess->font_query_detected = false;
    pap_session_rewind_spool(sess);
    if (sess->active && !sess->blocked_for_reply)
        pap_issue_senddata_request();
    return true;
}

// Handles the completion of a PatchPrep upload without terminating the session.
static void pap_handle_patch_complete(pap_session_t *sess) {
    if (!sess)
        return;
    LOG(2, "pap: PatchPrep upload complete; retaining session for job");
    sess->eof_pending = false;
    sess->patch_active = false;
    sess->expecting_patch = false;
    sess->patch_window_len = 0;
    sess->bytes_received = 0;
    sess->query_mode = PAP_QUERY_NONE;
    sess->font_query_window_len = 0;
    sess->font_query_detected = false;
    sess->query_detected = false;
    sess->query_waiting_job = false;
    sess->query_placeholder_eofs = 0;
    sess->query_window_len = 0;
    g_printer.patch_installed = true;
    // LaserWriter returns "1" after PatchPrep installs so the Mac skips re-sending it
    pap_queue_postscript_reply("1");
    pap_session_rewind_spool(sess);
    if (sess->active && !sess->blocked_for_reply)
        pap_issue_senddata_request();
}

// Closes the current spool, reports completion, and primes the session for another job.
static bool pap_finalize_job(const char *reason) {
    pap_session_t *sess = &g_session;
    if (!sess->active)
        return false;

    uint32_t completed_job = sess->job_id;
    char saved_path[PRINTER_SPOOL_PATH_MAX];
    saved_path[0] = '\0';
    if (sess->spool_path[0]) {
        strncpy(saved_path, sess->spool_path, sizeof(saved_path) - 1);
        saved_path[sizeof(saved_path) - 1] = '\0';
    }
    if (sess->spool) {
        fclose(sess->spool);
        sess->spool = NULL;
    }

    LOG(2, "pap: job %u complete (%s)", completed_job, reason ? reason : "done");
    if (saved_path[0])
        LOG(3, "pap: job %u captured at %s", completed_job, saved_path);

    pap_printer_set_status_idle();
    pap_status_queue_drain(PRINTER_STATUS_IDLE, false);

    sess->pending_reply_ready = false;
    sess->blocked_for_reply = false;
    sess->pending_reply[0] = '\0';
    sess->eof_pending = false;
    sess->bytes_received = 0;
    sess->query_detected = false;
    sess->query_waiting_job = false;
    sess->query_placeholder_eofs = 0;
    sess->expecting_patch = false;
    sess->patch_active = false;
    sess->query_mode = PAP_QUERY_NONE;
    sess->query_window_len = 0;
    sess->patch_window_len = 0;
    sess->font_query_window_len = 0;
    sess->font_query_detected = false;

    sess->spool_path[0] = '\0';
    sess->job_id = ++g_printer.job_counter;
    LOG(4, "pap: prepared for next job %u", sess->job_id);
    return true;
}

// Returns true when the fragment payload is just a "%%EOF" marker (optionally CR/LF).
static bool pap_fragment_is_placeholder_eof(const atp_response_fragment_t *fragment) {
    if (!fragment || fragment->data_len < 5 || fragment->data_len > 6)
        return false;
    const uint8_t *d = fragment->data;
    if (!d)
        return false;
    if (d[0] != '%' || d[1] != '%' || d[2] != 'E' || d[3] != 'O' || d[4] != 'F')
        return false;
    if (fragment->data_len == 6 && d[5] != '\r' && d[5] != '\n')
        return false;
    return true;
}

// Clears all queued SendData credits for the active session.
static void pap_status_queue_reset(void) {
    memset(g_session.pending_status_reads, 0, sizeof(g_session.pending_status_reads));
    g_session.pending_status_head = 0;
    g_session.pending_status_count = 0;
}

// Stores a SendData request so we can answer it when data becomes available.
static bool pap_status_queue_enqueue(const ddp_header_t *ddp, const atp_packet_t *atp) {
    if (!ddp || !atp)
        return false;
    if (g_session.pending_status_count >= PAP_MAX_FLOW_QUANTUM)
        return false;
    uint8_t idx = (uint8_t)((g_session.pending_status_head + g_session.pending_status_count) % PAP_MAX_FLOW_QUANTUM);
    pap_status_credit_t *slot = &g_session.pending_status_reads[idx];
    slot->in_use = true;
    slot->ddp = *ddp;
    slot->atp = *atp;
    slot->atp.data = NULL;
    slot->atp.data_len = 0;
    g_session.pending_status_count++;
    pap_log_session_state("queue-status");
    return true;
}

// Returns the oldest queued SendData credit, or NULL when none exist.
static pap_status_credit_t *pap_status_queue_head(void) {
    if (g_session.pending_status_count == 0)
        return NULL;
    return &g_session.pending_status_reads[g_session.pending_status_head];
}

// Removes the oldest queued SendData credit from the list.
static void pap_status_queue_pop(void) {
    if (g_session.pending_status_count == 0)
        return;
    pap_status_credit_t *slot = &g_session.pending_status_reads[g_session.pending_status_head];
    memset(slot, 0, sizeof(*slot));
    g_session.pending_status_head = (uint8_t)((g_session.pending_status_head + 1) % PAP_MAX_FLOW_QUANTUM);
    g_session.pending_status_count--;
}

// Formats a status or reply string with CRLF terminator for PAP reads.
static int pap_format_status_line(const char *text, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return 0;
    if (!text || !*text) {
        out[0] = '\0';
        return 0;
    }
    int len = snprintf(out, out_len, "%s\r\n", text);
    if (len < 0)
        len = 0;
    if ((size_t)len > out_len)
        len = (int)out_len;
    return len;
}

// Sends a stored reply string out on the oldest pending SendData credit.
static bool pap_try_deliver_pending_reply(void) {
    if (!g_session.pending_reply_ready)
        return false;
    pap_status_credit_t *credit = pap_status_queue_head();
    if (!credit)
        return false;
    char line[PRINTER_STATUS_MAX + 3];
    int len = pap_format_status_line(g_session.pending_reply, line, sizeof(line));
    LOG(2, "PAP -> Mac StatusData conn=%u bytes=%d source=postscript-reply", (unsigned)credit->atp.user[0], len);
    pap_send_data_response(&credit->ddp, &credit->atp, credit->atp.user[0], (len > 0) ? (const uint8_t *)line : NULL,
                           len, false);
    pap_status_queue_pop();
    g_session.pending_reply_ready = false;
    g_session.blocked_for_reply = false;
    pap_log_session_state("deliver-reply");
    if (g_session.active && !g_session.awaiting_data)
        pap_issue_senddata_request();
    return true;
}

// Flushes queued SendData credits with a specific text (or EOF) before teardown.
static void pap_status_queue_drain(const char *text, bool eof_on_last) {
    uint8_t queued = g_session.pending_status_count;
    LOG(4, "pap: draining %u status credits (eof_on_last=%d) text='%s'", (unsigned)queued, eof_on_last ? 1 : 0,
        text ? text : "<empty>");
    char line[PRINTER_STATUS_MAX + 3];
    int len = pap_format_status_line(text, line, sizeof(line));
    while (true) {
        pap_status_credit_t *credit = pap_status_queue_head();
        if (!credit)
            break;
        LOG(2, "PAP -> Mac StatusData conn=%u bytes=%d source=drain", (unsigned)credit->atp.user[0], len);
        pap_send_data_response(&credit->ddp, &credit->atp, credit->atp.user[0],
                               (len > 0) ? (const uint8_t *)line : NULL, len,
                               eof_on_last && (g_session.pending_status_count == 1));
        pap_status_queue_pop();
    }
}

// Logs the result of a CloseConn ATP request and releases its context.
static void pap_close_request_complete(atp_request_handle_t *handle, atp_request_result_t result, void *ctx) {
    (void)handle;
    pap_close_request_ctx_t *close_ctx = (pap_close_request_ctx_t *)ctx;
    if (!close_ctx)
        return;
    LOG(3, "pap: CloseConn complete conn=%u result=%d", (unsigned)close_ctx->conn_id, (int)result);
    free(close_ctx);
}

// Sends a CloseConn request to prompt the workstation to tear down the session.
static void pap_issue_close_conn(uint8_t conn_id, const atalk_socket_addr_t *addr) {
    if (!addr || addr->socket == 0 || conn_id == 0)
        return;
    uint8_t user[4] = {conn_id, PAP_FUNC_CLOSE, 0, 0};
    atp_request_params_t params = {.dest = *addr,
                                   .src_socket = HOST_PAP_SOCKET,
                                   .bitmap = 0x01,
                                   .mode = ATP_TRANSACTION_XO,
                                   .trel_timer_hint = 2,
                                   .user = {0},
                                   .payload = NULL,
                                   .payload_len = 0,
                                   .retry_timeout_ms = 2000,
                                   .retry_limit = 5};
    memcpy(params.user, user, sizeof(user));
    pap_close_request_ctx_t *ctx = (pap_close_request_ctx_t *)malloc(sizeof(pap_close_request_ctx_t));
    atp_request_callbacks_t callbacks = {0};
    if (ctx) {
        ctx->conn_id = conn_id;
        callbacks.on_complete = pap_close_request_complete;
    } else {
        LOG(1, "pap: unable to allocate CloseConn context for conn=%u", (unsigned)conn_id);
    }
    atp_request_handle_t *handle = atp_request_submit(&params, ctx ? &callbacks : NULL, ctx);
    if (!handle) {
        LOG(1, "pap: failed to submit CloseConn request for conn=%u", (unsigned)conn_id);
        if (ctx)
            free(ctx);
        return;
    }
    LOG(2, "pap: issued CloseConn conn=%u dstSock=%u", (unsigned)conn_id, (unsigned)addr->socket);
}

static void pap_completion_set(uint8_t conn_id, const char *text, const atalk_socket_addr_t *addr) {
    if (!text || conn_id == 0)
        return;
    size_t len = strlen(text);
    if (len > PRINTER_STATUS_MAX)
        len = PRINTER_STATUS_MAX;
    memcpy(g_completion.text, text, len);
    g_completion.text[len] = '\0';
    g_completion.conn_id = conn_id;
    g_completion.payload_sent = false;
    if (addr)
        g_completion.client_addr = *addr;
    else
        memset(&g_completion.client_addr, 0, sizeof(g_completion.client_addr));
    g_completion.active = true;
    LOG(2, "pap: queued completion reply for conn=%u", conn_id);
}

static void pap_completion_clear(void) {
    memset(&g_completion, 0, sizeof(g_completion));
}

// Emits a snapshot of the current session state to aid ATP/PAP debugging.
static void pap_log_session_state(const char *tag) {
    if (!g_session.active)
        return;
    LOG(5,
        "pap: state[%s] conn=%u job=%u awaiting=%d blocked=%d pendingCredits=%u inflightSeq=%u eofPending=%d "
        "replyReady=%d bytes=%zu",
        tag ? tag : "-", (unsigned)g_session.conn_id, (unsigned)g_session.job_id, g_session.awaiting_data ? 1 : 0,
        g_session.blocked_for_reply ? 1 : 0, (unsigned)g_session.pending_status_count, (unsigned)g_session.inflight_seq,
        g_session.eof_pending ? 1 : 0, g_session.pending_reply_ready ? 1 : 0, g_session.bytes_received);
}

// Sends a CloseConn request to the remote workstation to mirror LaserWriter behavior.
// Returns a readable label for a PAP function code or NULL when unknown.
static const char *pap_func_name(uint8_t func) {
    switch (func) {
    case PAP_FUNC_OPEN:
        return "OpenConn";
    case PAP_FUNC_SEND_STATUS:
        return "SendStatus";
    case PAP_FUNC_CLOSE:
        return "CloseConn";
    case PAP_FUNC_SENDDATA:
        return "StatusRead";
    case PAP_FUNC_TICKLE:
        return "Tickle";
    case PAP_FUNC_OPEN_REPLY:
        return "OpenReply";
    case PAP_FUNC_CLOSE_REPLY:
        return "CloseReply";
    case PAP_FUNC_STATUS:
        return "StatusReply";
    case PAP_FUNC_DATA:
        return "Data";
    default:
        return NULL;
    }
}

// Builds a short suffix with request-specific PAP fields for tracing.
static void pap_format_request_detail(char *dst, size_t dst_len, uint8_t func, const atp_packet_t *packet) {
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (!packet)
        return;
    switch (func) {
    case PAP_FUNC_OPEN:
        if (packet->data_len >= 2) {
            uint8_t client_sock = packet->data[0];
            uint8_t flow = packet->data[1] ? packet->data[1] : PAP_MAX_FLOW_QUANTUM;
            if (packet->data_len >= 4) {
                uint8_t status_sock = packet->data[2];
                uint8_t status_interval = packet->data[3];
                snprintf(dst, dst_len, "; clientSock=%u flowQuantum=%u statusSock=%u statusInterval=%u",
                         (unsigned)client_sock, (unsigned)flow, (unsigned)status_sock, (unsigned)status_interval);
            } else {
                snprintf(dst, dst_len, "; clientSock=%u flowQuantum=%u", (unsigned)client_sock, (unsigned)flow);
            }
        }
        break;
    default:
        break;
    }
}

static void pap_send_data_response(const ddp_header_t *ddp, atp_packet_t *atp, uint8_t conn_id, const uint8_t *payload,
                                   int len, bool set_eof) {
    uint8_t user[4] = {conn_id, PAP_FUNC_DATA, set_eof ? 1 : 0, 0};
    atp_response_packet_desc_t desc = {
        .payload = payload, .payload_len = (len > 0) ? len : 0, .user = user, .sts = false, .eom = true};
    atp_responder_send_packets(ddp, atp, &desc, 1);
}

// Issues a PAP SendData request (PAPRead) to pull PostScript from the workstation.
static bool pap_issue_senddata_request(void) {
    if (!g_session.active) {
        LOG(6, "pap: skipping SendData issue (session inactive)");
        return false;
    }
    if (g_session.awaiting_data) {
        LOG(6, "pap: skipping SendData issue (awaiting response for seq=%u)", (unsigned)g_session.inflight_seq);
        return false;
    }
    if (g_session.blocked_for_reply) {
        LOG(6, "pap: skipping SendData issue (blocked_for_reply pendingCredits=%u)",
            (unsigned)g_session.pending_status_count);
        return false;
    }

    uint8_t quantum = g_session.server_flow_quantum ? g_session.server_flow_quantum : PAP_MAX_FLOW_QUANTUM;
    if (quantum > PAP_MAX_FLOW_QUANTUM)
        quantum = PAP_MAX_FLOW_QUANTUM;
    uint8_t bitmap = (quantum >= 8) ? 0xFF : (uint8_t)((1u << quantum) - 1u);
    if (bitmap == 0)
        bitmap = 1;

    uint16_t seq = g_session.next_send_seq++;
    if (g_session.next_send_seq == 0)
        g_session.next_send_seq = 1;

    uint8_t user[4] = {g_session.conn_id, PAP_FUNC_SENDDATA, (uint8_t)(seq >> 8), (uint8_t)(seq & 0xFF)};

    atp_request_params_t params = {.dest = g_session.client_addr,
                                   .src_socket = HOST_PAP_SOCKET,
                                   .bitmap = bitmap,
                                   .mode = ATP_TRANSACTION_XO,
                                   .trel_timer_hint = 2,
                                   .user = {0},
                                   .payload = NULL,
                                   .payload_len = 0,
                                   .retry_timeout_ms = PAP_SENDDATA_RETRY_MS,
                                   .retry_limit = PAP_SENDDATA_RETRY_LIMIT};
    memcpy(params.user, user, sizeof(user));

    atp_request_callbacks_t callbacks = {.on_response = pap_handle_data_fragment,
                                         .on_complete = pap_handle_data_complete};

    g_session.send_handle = atp_request_submit(&params, &callbacks, &g_session);
    if (!g_session.send_handle) {
        LOG(1, "pap: failed to submit SendData request");
        pap_session_abort("senddata submit");
        return false;
    }

    g_session.awaiting_data = true;
    g_session.inflight_seq = seq;
    pap_session_record_activity();
    LOG(2, "pap: SendData conn=%u seq=%u bitmap=0x%02X pendingCredits=%u", g_session.conn_id, seq, bitmap,
        (unsigned)g_session.pending_status_count);
    pap_log_session_state("senddata-issue");
    return true;
}

// ATP response callback that writes each data fragment to the spool file.
static void pap_handle_data_fragment(const atp_response_fragment_t *fragment, void *ctx) {
    pap_session_t *sess = (pap_session_t *)ctx;
    if (!sess || !sess->active || !fragment)
        return;
    pap_session_record_activity();
    LOG(5, "pap: fragment seq=%u len=%d dup=%d eom=%d sts=%d bitmapRemaining=0x%02X", (unsigned)fragment->seq,
        fragment->data_len, fragment->duplicate ? 1 : 0, fragment->eom ? 1 : 0, fragment->sts ? 1 : 0,
        (unsigned)fragment->bitmap_remaining);
    if (fragment->duplicate)
        return;
    bool placeholder_eof = false;
    if (sess->query_waiting_job && fragment->user[2])
        placeholder_eof = pap_fragment_is_placeholder_eof(fragment);

    bool patch_token = false;
    bool font_query_token = false;
    if (fragment->data_len > 0 && fragment->data) {
        patch_token = pap_detect_patchprep_sequence(sess, fragment->data, fragment->data_len);
        font_query_token = pap_detect_fontlist_sequence(sess, fragment->data, fragment->data_len);
    }
    if (patch_token && sess->query_detected && sess->query_waiting_job && !sess->expecting_patch &&
        !g_printer.patch_installed) {
        sess->expecting_patch = true;
        LOG(2, "pap: PatchPrep query detected; expecting upload");
    }
    if (font_query_token && !sess->font_query_detected) {
        sess->font_query_detected = true;
        sess->query_mode = PAP_QUERY_FONT_LIST;
        LOG(2, "pap: detected PostScript font list query prolog");
    }

    if (placeholder_eof) {
        sess->query_placeholder_eofs++;
        LOG(3, "pap: ignoring placeholder EOF while waiting for job (count=%u)",
            (unsigned)sess->query_placeholder_eofs);
        if (sess->query_placeholder_eofs >= PAP_QUERY_PLACEHOLDER_LIMIT) {
            LOG(1, "pap: placeholder EOF limit reached; finishing session");
            sess->query_waiting_job = false;
            sess->query_placeholder_eofs = 0;
            sess->eof_pending = true;
        }
    } else if (fragment->data_len > 0) {
        if (!sess->spool) {
            if (!pap_session_open_spool(sess)) {
                pap_session_abort("spool open failed");
                return;
            }
        }
        size_t wrote = fwrite(fragment->data, 1, (size_t)fragment->data_len, sess->spool);
        if (wrote != (size_t)fragment->data_len)
            LOG(1, "pap: short write to spool (%zu vs %d)", wrote, fragment->data_len);
        sess->bytes_received += wrote;
        pap_update_progress_status();
        bool flush_sequence = pap_detect_flush_sequence(sess, fragment->data, fragment->data_len);
        if (!sess->query_detected && flush_sequence) {
            sess->query_detected = true;
            sess->query_waiting_job = true;
            sess->query_placeholder_eofs = 0;
            if (sess->font_query_detected) {
                sess->query_mode = PAP_QUERY_FONT_LIST;
                LOG(2, "pap: detected font list query; sending built-in names");
                pap_queue_font_list_reply();
            } else {
                sess->query_mode = PAP_QUERY_PATCH_STATUS;
                const char *patch_reply = g_printer.patch_installed ? "1" : "0";
                LOG(2, "pap: detected query flush sequence; reply=%s (patch_installed=%d)", patch_reply,
                    g_printer.patch_installed ? 1 : 0);
                pap_queue_postscript_reply(patch_reply);
            }
        } else if (sess->query_waiting_job && sess->query_mode != PAP_QUERY_FONT_LIST) {
            sess->query_waiting_job = false;
            sess->query_placeholder_eofs = 0;
            sess->query_mode = PAP_QUERY_NONE;
            if (sess->expecting_patch && !sess->patch_active) {
                sess->patch_active = true;
                LOG(2, "pap: PatchPrep payload upload in progress");
            } else {
                LOG(2, "pap: real job payload detected after query");
            }
        }
    }
    if (fragment->user[2] && !placeholder_eof)
        sess->eof_pending = true;
}

// ATP completion callback that advances to the next SendData or finalizes the job.
static void pap_handle_data_complete(atp_request_handle_t *handle, atp_request_result_t result, void *ctx) {
    pap_session_t *sess = (pap_session_t *)ctx;
    if (!sess)
        return;
    if (sess->send_handle == handle)
        sess->send_handle = NULL;
    sess->awaiting_data = false;
    uint16_t completed_seq = sess->inflight_seq;
    sess->inflight_seq = 0;
    pap_session_record_activity();
    LOG(4, "pap: SendData complete seq=%u result=%d eof_pending=%d blocked=%d pendingCredits=%u",
        (unsigned)completed_seq, (int)result, sess->eof_pending ? 1 : 0, sess->blocked_for_reply ? 1 : 0,
        (unsigned)sess->pending_status_count);
    pap_log_session_state("senddata-complete");

    switch (result) {
    case ATP_REQUEST_RESULT_OK:
        if (sess->eof_pending) {
            if (pap_consume_query_eof(sess))
                break;
            if (sess->patch_active) {
                pap_handle_patch_complete(sess);
                break;
            }
            if (pap_finalize_job("EOF received") && sess->active && !sess->blocked_for_reply)
                pap_issue_senddata_request();
            break;
        }
        if (sess->active && !sess->blocked_for_reply) {
            pap_issue_senddata_request();
        }
        break;
    case ATP_REQUEST_RESULT_TIMEOUT:
        LOG(1, "pap: SendData timeout (seq=%u)", (unsigned)completed_seq);
        pap_session_abort("senddata timeout");
        break;
    case ATP_REQUEST_RESULT_ABORTED:
        LOG(2, "pap: SendData aborted");
        break;
    }
}

// Handles incoming PAP OpenConn requests.
static void pap_handle_open(const ddp_header_t *ddp, atp_packet_t *atp) {
    uint8_t conn_id = atp->user[0];
    uint16_t result = PAP_RESULT_OK;
    bool accepted = false;

    if (!g_printer.enabled) {
        result = PAP_RESULT_BUSY;
    } else if (g_session.active) {
        result = PAP_RESULT_BUSY;
    } else if (!atp || atp->data_len < 4) {
        result = PAP_RESULT_BUSY;
    } else {
        pap_session_reset();
        g_session.active = true;
        g_session.conn_id = conn_id;
        g_session.client_socket = atp->data[0];
        g_session.client_flow_quantum = atp->data[1] ? atp->data[1] : PAP_MAX_FLOW_QUANTUM;
        g_session.server_flow_quantum = PAP_MAX_FLOW_QUANTUM;
        g_session.client_addr.net = ddp->src_net;
        g_session.client_addr.node = ddp->llap.src;
        g_session.client_addr.socket = g_session.client_socket;
        g_session.job_id = ++g_printer.job_counter;
        g_session.next_send_seq = 1;
        g_session.bytes_received = 0;
        g_session.last_activity_ms = pap_now_ms();
        if (!pap_session_open_spool(&g_session)) {
            result = PAP_RESULT_BUSY;
            pap_session_reset();
        } else {
            accepted = true;
            pap_update_progress_status();
        }
    }

    uint8_t payload[5 + PRINTER_STATUS_MAX];
    int payload_len = pap_build_status_payload(HOST_PAP_SOCKET, PAP_MAX_FLOW_QUANTUM, result, payload, sizeof(payload));
    if (payload_len < 0)
        payload_len = 0;
    uint8_t user[4] = {conn_id, PAP_FUNC_OPEN_REPLY, 0, 0};
    LOG(2, "PAP -> Mac OpenReply conn=%u result=0x%04X statusLen=%u", conn_id, (unsigned)result,
        (unsigned)g_printer.status_len);
    atp_responder_send_simple(ddp, atp, user, payload_len ? payload : NULL, payload_len, false);

    if (accepted)
        pap_issue_senddata_request();
}

// Handles PAP SendStatus requests (ConnID must be zero per spec).
static void pap_handle_send_status(const ddp_header_t *ddp, atp_packet_t *atp) {
    uint8_t payload[5 + PRINTER_STATUS_MAX];
    int payload_len =
        pap_build_status_payload(HOST_PAP_SOCKET, PAP_MAX_FLOW_QUANTUM, PAP_RESULT_OK, payload, sizeof(payload));
    if (payload_len < 0)
        payload_len = 0;
    uint8_t user[4] = {0, PAP_FUNC_STATUS, 0, 0};
    LOG(2, "PAP -> Mac SendStatus reply statusLen=%u", (unsigned)g_printer.status_len);
    atp_responder_send_simple(ddp, atp, user, payload_len ? payload : NULL, payload_len, false);
}

// Handles PAP CloseConn requests by acknowledging and finalizing the job.
static void pap_handle_close_conn(const ddp_header_t *ddp, atp_packet_t *atp) {
    uint8_t user[4] = {atp->user[0], PAP_FUNC_CLOSE_REPLY, 0, 0};
    LOG(2, "PAP -> Mac CloseReply conn=%u", (unsigned)atp->user[0]);
    atp_responder_send_simple(ddp, atp, user, NULL, 0, false);
    pap_session_finish(true, "client closed", false);
}

// Handles PAP SendData requests issued by the workstation to read status text.
static void pap_handle_status_read(const ddp_header_t *ddp, atp_packet_t *atp) {
    uint8_t conn_id = atp->user[0];
    bool same_conn = g_session.active && conn_id == g_session.conn_id;
    if (same_conn)
        pap_session_record_activity();

    uint16_t seq = (uint16_t)((atp->user[2] << 8) | atp->user[3]);
    bool completion_match = (!same_conn && g_completion.active && conn_id == g_completion.conn_id);

    if (same_conn) {
        if (!pap_status_queue_enqueue(ddp, atp)) {
            LOG(1, "pap: status credit overflow conn=%u seq=%u", (unsigned)conn_id, (unsigned)seq);
            pap_send_data_response(ddp, atp, conn_id, NULL, 0, false);
        } else {
            LOG(3, "pap: queued SendData credit conn=%u seq=%u depth=%u", (unsigned)conn_id, (unsigned)seq,
                (unsigned)g_session.pending_status_count);
            pap_try_deliver_pending_reply();
        }
        return;
    }

    // When no session/completion is associated with this conn, immediately signal EOF with an empty reply.
    if (!completion_match) {
        LOG(3, "pap: answering stale SendData conn=%u with empty EOF", (unsigned)conn_id);
        pap_send_data_response(ddp, atp, conn_id, NULL, 0, true);
        return;
    }

    if (completion_match) {
        if (!g_completion.payload_sent) {
            char line[PRINTER_STATUS_MAX + 3];
            int len = pap_format_status_line(g_completion.text, line, sizeof(line));
            LOG(2, "PAP -> Mac StatusData conn=%u bytes=%d source=completion", (unsigned)conn_id, len);
            pap_send_data_response(ddp, atp, conn_id, (len > 0) ? (const uint8_t *)line : NULL, len, true);
            g_completion.payload_sent = true;
            pap_completion_clear();
        } else {
            LOG(2, "pap: answering duplicate completion SendData conn=%u with empty EOF", (unsigned)conn_id);
            pap_send_data_response(ddp, atp, conn_id, NULL, 0, true);
        }
        return;
    }
}

// Handles PAP tickle packets to keep the session alive.
static void pap_handle_tickle(void) {
    pap_session_record_activity();
}

// Central ATP socket handler for PAP requests arriving on HOST_PAP_SOCKET.
static void pap_socket_request_handler(const ddp_header_t *ddp, atp_packet_t *request, void *ctx) {
    (void)ctx;
    pap_printer_init();
    if (!ddp || !request)
        return;

    pap_session_check_timeout();

    uint8_t func = request->user[1];
    char detail[96];
    pap_format_request_detail(detail, sizeof(detail), func, request);
    const char *func_name = pap_func_name(func);
    if (func_name) {
        LOG(2, "PAP <- Mac %s: conn=%u tid=0x%04X bitmap=0x%02X len=%d%s", func_name, (unsigned)request->user[0],
            request->tid, request->bitmap, request->data_len, detail);
    } else {
        LOG(2, "PAP <- Mac func=%u: conn=%u tid=0x%04X bitmap=0x%02X len=%d%s", func, (unsigned)request->user[0],
            request->tid, request->bitmap, request->data_len, detail);
    }

    LOG_INDENT(4);
    switch (func) {
    case PAP_FUNC_OPEN:
        pap_handle_open(ddp, request);
        break;
    case PAP_FUNC_SEND_STATUS:
        pap_handle_send_status(ddp, request);
        break;
    case PAP_FUNC_CLOSE:
        pap_handle_close_conn(ddp, request);
        break;
    case PAP_FUNC_SENDDATA:
        pap_handle_status_read(ddp, request);
        break;
    case PAP_FUNC_TICKLE:
        pap_handle_tickle();
        break;
    default:
        LOG(3, "pap: unhandled function %u", func);
        break;
    }
    LOG_INDENT(-4);
}

// Registers the printer PAP socket handler and auto-enables the LaserWriter advertisement.
void atalk_printer_register(void) {
    pap_printer_init();
    static const atp_socket_handler_t handler = {.handle_request = pap_socket_request_handler};
    atp_register_socket_handler(HOST_PAP_SOCKET, &handler, NULL);
    if (!g_printer.enabled) {
        if (atalk_printer_enable(NULL) != 0)
            LOG(1, "pap: failed to auto-enable printer");
    }
}

// Enables (or renames) the emulated LaserWriter and registers its NBP entry.
int atalk_printer_enable(const char *object_name) {
    pap_printer_init();
    if (object_name && *object_name) {
        size_t n = strlen(object_name);
        if (n > PRINTER_OBJECT_MAX)
            n = PRINTER_OBJECT_MAX;
        memcpy(g_printer.object_name, object_name, n);
        g_printer.object_name[n] = '\0';
    }

    atalk_nbp_service_desc_t desc = {.object = g_printer.object_name,
                                     .type = PRINTER_ENTITY_TYPE,
                                     .zone = "*",
                                     .socket = HOST_PAP_SOCKET,
                                     .node = LLAP_HOST_NODE,
                                     .net = 0};

    int rc;
    if (g_printer.nbp_entry)
        rc = atalk_nbp_update(g_printer.nbp_entry, &desc);
    else
        rc = atalk_nbp_register(&desc, &g_printer.nbp_entry);

    if (rc != 0) {
        LOG(1, "pap: failed to register printer NBP entry");
        printf("atalk: failed to publish printer '%s'\n", g_printer.object_name);
        return -1;
    }

    g_printer.enabled = true;
    pap_printer_set_status_idle();
    LOG(1, "atalk: printer enabled as '%s'", g_printer.object_name);
    return 0;
}

// Disables the LaserWriter advertisement and aborts any active job.
int atalk_printer_disable(void) {
    pap_printer_init();
    if (g_printer.nbp_entry) {
        atalk_nbp_unregister(g_printer.nbp_entry);
        g_printer.nbp_entry = NULL;
    }
    g_printer.enabled = false;
    pap_session_abort("printer disabled");
    pap_printer_set_status_disabled();
    printf("atalk: printer disabled\n");
    return 0;
}
