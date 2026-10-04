// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iw_printer.c
// The virtual ImageWriter device: configuration, connection, jobs, the
// object model and the checkpoint.  See iw_printer.h.
//
// Serial bytes arrive synchronously inside the guest's write to the SCC's
// data register (the SCC transmits instantly), so the port callback only
// queues them; a scheduler event ~1 ms of guest time later runs the
// interpreter over what has arrived.  A job ends after `idle_timeout_ms` of
// guest time with no input -- many drivers never send a final form feed --
// so everything is guest-time driven and a checkpoint restores to the same
// state.

#include "iw_printer.h"

#include "byteq.h"
#include "crc32.h"
#include "iw_interp.h"
#include "log.h"
#include "machine_profile.h"
#include "object.h"
#include "pdf_writer.h"
#include "printer_sink.h"
#include "scheduler.h"
#include "system.h"
#include "value.h"
#include "vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("imagewriter");

// Input waiting for the interpreter: far more than any serial stream
// accumulates in a millisecond, bounded so a runaway guest cannot exhaust
// memory
#define IW_INPUT_MAX (4u * 1024u * 1024u)
// A job's captured input (the `capture` switch)
#define IW_CAPTURE_MAX (32u * 1024u * 1024u)
// Guest time from the first queued byte to its interpretation
#define IW_PROCESS_DELAY_NS 1000000ull
// Guest time between the bytes of a reply (looks like a serial line, not a burst)
#define IW_REPLY_BYTE_NS 1000000ull
// A run of this many CANs at the start of a line starts a new job (the Lisa
// Office System opens every job with one)
#define IW_CAN_JOB_RUN 16
// Checkpoint layout version of this part
#define IW_CP_VERSION      1
#define IW_DEFAULT_IDLE_MS 5000u

// Paper choices
typedef struct {
    const char *name;
    double width_in, height_in;
    double printable_in; // the carriage width
    double left_offset_in;
} iw_paper_t;

static const iw_paper_t papers[] = {
    {"fanfold-letter", 8.5,  11.0,  8.0,  0.25 },
    {"letter",         8.5,  11.0,  8.0,  0.25 },
    {"a4",             8.27, 11.69, 8.0,  0.135},
    {"legal",          8.5,  14.0,  8.0,  0.25 },
    {"fanfold-15in",   14.0, 11.0,  13.6, 0.2  },
};
#define N_PAPERS (sizeof(papers) / sizeof(papers[0]))

static const char *const connection_names[IW_CONN_COUNT] = {"none", "serial-a", "serial-b", "localtalk"};

// Settings a checkpoint carries as one block
typedef struct {
    iw_config_t icfg;
    uint8_t connection; // iw_connection_t
    uint8_t paper; // index into papers[]
    bool capture;
    bool paper_out;
    bool panel_deselected; // the front panel's SELECT is off
    uint32_t idle_timeout_ms;
    uint8_t inks[4][3]; // Y, M, C, K ink colours
} iw_settings_t;

// Job and counter state a checkpoint carries as one block
typedef struct {
    bool job_active;
    uint32_t job_id; // the current (or last) job's number
    uint32_t jobs; // documents produced
    uint32_t pages; // pages in all documents
    uint64_t bytes; // input bytes received
    uint32_t last_job_pages;
    uint32_t last_pdf_crc;
    uint32_t last_pdf_len;
    char last_outcome[64];
    double last_rx_ns; // guest time of the last byte (idle timeout)
    uint32_t can_run; // consecutive CANs at the start of a line
    uint8_t reply[16]; // bytes queued for the host (ESC ?, XON/XOFF)
    uint8_t reply_len, reply_pos;
    bool xoff_sent; // XON/XOFF: DC3 sent, DC1 owed
} iw_job_state_t;

struct iw_printer {
    struct scheduler *scheduler;
    scc_t *scc;
    iw_port_wiring_t wiring[2];
    bool wired_by_us[2]; // ready lines this printer wired (and unwires)
    iw_settings_t set;
    iw_job_state_t job;
    iw_interp_t interp;
    byteq_t input; // serial bytes not yet interpreted
    byteq_t capture; // this job's input, when capturing
    pdf_writer_t *pdf; // this job's document, from its first page
    char status[64];
    struct object *object;
};

// The machine's printer when it sits on LocalTalk
static iw_printer_t *g_localtalk;

static const class_desc_t iw_printer_class;
static void process_event(void *source, uint64_t data);
static void idle_event(void *source, uint64_t data);
static void reply_event(void *source, uint64_t data);
static void update_ready(iw_printer_t *p);

// Deselected at the front panel or out of paper: the printer stops printing.
// What it has already received stays in its buffer, and the job waits.
static bool paused(const iw_printer_t *p) {
    return p->set.panel_deselected || p->set.paper_out;
}

// --- Names ---------------------------------------------------------------------

const char *iw_printer_name(const iw_printer_t *p) {
    return p->set.icfg.model == IW_MODEL_IW1 ? "ImageWriter" : "ImageWriter II";
}

static const char *printer_slug(const iw_printer_t *p) {
    return p->set.icfg.model == IW_MODEL_IW1 ? "imagewriter" : "imagewriter2";
}

// --- Status --------------------------------------------------------------------

// Recompose the status text and tell the platform when it changed.
static void update_status(iw_printer_t *p) {
    char s[64];
    if (p->set.paper_out)
        snprintf(s, sizeof(s), "status: out of paper");
    else if (p->set.panel_deselected || !p->interp.st.selected)
        snprintf(s, sizeof(s), "status: deselected");
    else if (p->job.job_active)
        snprintf(s, sizeof(s), "status: printing; page: %u", (unsigned)p->interp.st.pages_done + 1);
    else
        snprintf(s, sizeof(s), "status: idle");
    if (strcmp(s, p->status) == 0)
        return;
    memcpy(p->status, s, sizeof(s));
    printer_sink_status(iw_printer_name(p), p->status);
}

const char *iw_printer_status(const iw_printer_t *p) {
    return p->status;
}

bool iw_printer_paper_out(const iw_printer_t *p) {
    return p->set.paper_out;
}

bool iw_printer_selected(const iw_printer_t *p) {
    return !p->set.panel_deselected && p->interp.st.selected;
}

bool iw_printer_sheet_feeder(const iw_printer_t *p) {
    return p->set.icfg.sheet_feeder;
}

bool iw_printer_busy(const iw_printer_t *p) {
    return p->job.job_active;
}

// --- Configuration -------------------------------------------------------------

// Apply the chosen paper to the interpreter's configuration.
static void apply_paper(iw_printer_t *p) {
    const iw_paper_t *pp = &papers[p->set.paper < N_PAPERS ? p->set.paper : 0];
    p->set.icfg.paper_width_in = pp->width_in;
    p->set.icfg.paper_height_in = pp->height_in;
    p->set.icfg.printable_width_in = pp->printable_in;
    p->set.icfg.left_offset_in = pp->left_offset_in;
}

// Factory settings for `model`.
static void default_settings(iw_settings_t *s, iw_model_t model) {
    memset(s, 0, sizeof(*s));
    s->icfg.model = model;
    // IW II: 12 cpi (SW 1-6 closed), 9600 baud hardware handshake.  IW I: the
    // same switch positions (elite is its power-on pitch anyway).
    s->icfg.dip1 = 0x20;
    s->icfg.dip2 = IW_DIP2_BAUD;
    s->icfg.color_ribbon = model == IW_MODEL_IW2;
    s->icfg.sheet_feeder = false;
    // Where the print line sits below the sheet's top edge at top of form.
    // The Technical Reference recommends half an inch ("roll the paper so
    // that the top of the page is one half inch above the print head"); the
    // drivers assume a little more -- the Lisa Office System's driver 80/144
    // in, and the Macintosh driver backs the paper up 76/144 in at the start
    // of a job -- so 80 keeps both on the sheet they mean.
    s->icfg.tof_offset_144 = 80;
    s->icfg.cut_sheet = false;
    s->icfg.dpi = 288;
    s->icfg.dot_shape = IW_DOT_DISC;
    s->paper = 0;
    s->idle_timeout_ms = IW_DEFAULT_IDLE_MS;
    // A fresh ribbon, approximately
    static const uint8_t inks[4][3] = {
        {0xF2, 0xD2, 0x1C},
        {0xC8, 0x28, 0x7A},
        {0x1E, 0x78, 0xC8},
        {0x14, 0x14, 0x14}
    };
    memcpy(s->inks, inks, sizeof(inks));
}

// --- Jobs ------------------------------------------------------------------------

// A sheet is finished: add it to the job's document.
static void on_page_done(void *ctx, const iw_page_t *page) {
    iw_printer_t *p = ctx;
    if (!p->pdf) {
        p->pdf = pdf_writer_new("Print", "Granny Smith");
        if (!p->pdf) {
            LOG(1, "out of memory starting job %u's document", (unsigned)p->job.job_id);
            return;
        }
    }
    uint8_t palette[16][3];
    iw_page_palette(p->set.inks, palette);
    if (!iw_page_emit(page, p->pdf, palette))
        LOG(1, "job %u: page could not be added to the document", (unsigned)p->job.job_id);
    update_status(p);
}

// Queue `len` bytes for the host and start sending them.
static void queue_reply(iw_printer_t *p, const uint8_t *data, size_t len) {
    iw_job_state_t *j = &p->job;
    // Compact what is already sent
    if (j->reply_pos) {
        memmove(j->reply, j->reply + j->reply_pos, (size_t)(j->reply_len - j->reply_pos));
        j->reply_len = (uint8_t)(j->reply_len - j->reply_pos);
        j->reply_pos = 0;
    }
    for (size_t i = 0; i < len && j->reply_len < sizeof(j->reply); i++)
        j->reply[j->reply_len++] = data[i];
    if (p->scheduler && !has_event(p->scheduler, reply_event))
        scheduler_new_cpu_event(p->scheduler, reply_event, p, 0, 0, IW_REPLY_BYTE_NS);
}

// The interpreter answers (ESC ?): over a serial port, byte by byte.  Over
// AppleTalk nothing carries it.
static void on_reply(void *ctx, const uint8_t *data, size_t len) {
    iw_printer_t *p = ctx;
    if (p->set.connection != IW_CONN_SERIAL_A && p->set.connection != IW_CONN_SERIAL_B)
        return;
    queue_reply(p, data, len);
}

static void on_select(void *ctx, bool selected) {
    iw_printer_t *p = ctx;
    (void)selected;
    update_ready(p);
    update_status(p);
}

// Start a job: the first byte after idle.
static void job_begin(iw_printer_t *p) {
    iw_job_state_t *j = &p->job;
    j->job_active = true;
    j->job_id++;
    byteq_clear(&p->capture);
    iw_interp_begin_job(&p->interp);
    LOG(2, "job %u started", (unsigned)j->job_id);
    update_status(p);
}

// End the job: finish its sheets, hand the document and the capture over.
static void job_end(iw_printer_t *p) {
    iw_job_state_t *j = &p->job;
    if (!j->job_active)
        return;
    iw_interp_end_job(&p->interp);
    j->job_active = false;
    if (p->scheduler)
        remove_event(p->scheduler, idle_event, p);

    if (p->set.capture && byteq_len(&p->capture)) {
        printer_capture_t cap = {.slug = printer_slug(p),
                                 .job_id = j->job_id,
                                 .ext = "iw",
                                 .data = byteq_data(&p->capture),
                                 .len = byteq_len(&p->capture)};
        printer_sink_capture(&cap);
    }
    byteq_clear(&p->capture);

    uint32_t pages = p->pdf ? pdf_writer_pages(p->pdf) : 0;
    if (!pages) {
        // Nothing reached paper (a status query, settings only): no document
        LOG(2, "job %u ended with nothing printed", (unsigned)j->job_id);
        // Its number goes to the next job: documents are numbered in order
        j->job_id--;
        pdf_writer_free(p->pdf);
        p->pdf = NULL;
        update_status(p);
        return;
    }
    uint8_t *pdf = NULL;
    size_t pdf_len = 0;
    bool ok = pdf_writer_finish(p->pdf, &pdf, &pdf_len);
    pdf_writer_free(p->pdf);
    p->pdf = NULL;
    if (!ok) {
        snprintf(j->last_outcome, sizeof(j->last_outcome), "failed: out of memory");
        LOG(1, "job %u: the document could not be written", (unsigned)j->job_id);
        update_status(p);
        return;
    }
    j->jobs++;
    j->pages += pages;
    j->last_job_pages = pages;
    j->last_pdf_crc = gs_crc32(0, pdf, pdf_len);
    j->last_pdf_len = (uint32_t)pdf_len;
    snprintf(j->last_outcome, sizeof(j->last_outcome), "%s",
             p->interp.st.placeholder_used ? "ok (placeholder font)" : "ok");
    printer_document_t doc = {.printer = iw_printer_name(p),
                              .slug = printer_slug(p),
                              .job_id = j->job_id,
                              .title = "Print",
                              .pdf = pdf,
                              .pdf_len = pdf_len,
                              .pages = pages,
                              .ok = true,
                              .detail = ""};
    LOG(2, "job %u: %u page(s), %zu bytes", (unsigned)j->job_id, (unsigned)pages, pdf_len);
    printer_sink_deliver(&doc);
    free(pdf);
    update_status(p);
}

void iw_printer_end_job(iw_printer_t *p) {
    if (!p)
        return;
    // What is still queued belongs to this job
    if (byteq_len(&p->input))
        process_event(p, 0);
    job_end(p);
}

// Interpret `len` bytes, starting a job on the first and splitting jobs at a
// long CAN run.
static void interpret(iw_printer_t *p, const uint8_t *data, size_t len) {
    iw_job_state_t *j = &p->job;
    if (len == 0)
        return;
    if (!j->job_active)
        job_begin(p);
    j->bytes += len;
    if (p->set.capture && !byteq_append(&p->capture, data, len, IW_CAPTURE_MAX))
        LOG(1, "job %u: capture full, input no longer captured", (unsigned)j->job_id);
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        bool normal = p->interp.st.ps == IW_PS_NORMAL;
        if (data[i] == 0x18 && normal) {
            // A long CAN run on a job that has printed: the next job's start
            if (++j->can_run == IW_CAN_JOB_RUN && (p->pdf || iw_page_marked(&p->interp.page) || p->interp.st.n_marks)) {
                iw_interp_feed(&p->interp, data + start, i - start);
                start = i;
                job_end(p);
                job_begin(p);
            }
        } else if (normal || data[i] != 0x18) {
            j->can_run = 0;
        }
    }
    iw_interp_feed(&p->interp, data + start, len - start);
    update_status(p);
}

void iw_printer_feed(iw_printer_t *p, const uint8_t *data, size_t len) {
    if (!p || !len)
        return;
    // Bytes already queued from a serial port go first
    if (byteq_len(&p->input))
        process_event(p, 0);
    interpret(p, data, len);
}

// --- Scheduler events ------------------------------------------------------------

// Run the interpreter over the queued input.
static void process_event(void *source, uint64_t data) {
    (void)data;
    iw_printer_t *p = source;
    size_t n = byteq_len(&p->input);
    if (!n || paused(p))
        return; // resumed (resume_input) when the printer is ready again
    // Copy out first: interpreting may end a job, which must not see the queue change
    uint8_t *buf = malloc(n);
    if (!buf)
        return;
    byteq_read(&p->input, buf, n);
    interpret(p, buf, n);
    free(buf);
}

// The input has been quiet: end the job, or look again when it was not quiet
// long enough.
static void idle_event(void *source, uint64_t data) {
    (void)data;
    iw_printer_t *p = source;
    if (!p->job.job_active)
        return;
    double timeout = (double)p->set.idle_timeout_ms * 1e6;
    // A paused printer is not idle: its job waits for the operator
    if (paused(p)) {
        scheduler_new_cpu_event(p->scheduler, idle_event, p, 0, 0, (uint64_t)timeout);
        return;
    }
    double quiet = scheduler_time_ns(p->scheduler) - p->job.last_rx_ns;
    if (quiet + 1.0 < timeout || byteq_len(&p->input)) {
        double left = timeout - quiet;
        scheduler_new_cpu_event(p->scheduler, idle_event, p, 0, 0, (uint64_t)(left > 1e6 ? left : 1e6));
        return;
    }
    job_end(p);
}

// Send the next reply byte to the host.
static void reply_event(void *source, uint64_t data) {
    (void)data;
    iw_printer_t *p = source;
    iw_job_state_t *j = &p->job;
    if (j->reply_pos >= j->reply_len) {
        j->reply_pos = j->reply_len = 0;
        return;
    }
    unsigned ch = p->set.connection == IW_CONN_SERIAL_A ? 0 : 1;
    if (p->set.connection == IW_CONN_SERIAL_A || p->set.connection == IW_CONN_SERIAL_B)
        scc_port_rx_byte(p->scc, ch, j->reply[j->reply_pos]);
    j->reply_pos++;
    if (j->reply_pos < j->reply_len)
        scheduler_new_cpu_event(p->scheduler, reply_event, p, 0, 0, IW_REPLY_BYTE_NS);
    else
        j->reply_pos = j->reply_len = 0;
}

// --- The serial port ----------------------------------------------------------------

// A byte from the guest, inside its write to the SCC: queue it.
static void port_tx_byte(void *ctx, uint8_t byte) {
    iw_printer_t *p = ctx;
    if (!byteq_append(&p->input, &byte, 1, IW_INPUT_MAX)) {
        LOG(1, "input queue full: byte dropped");
        return;
    }
    p->job.last_rx_ns = scheduler_time_ns(p->scheduler);
    if (!has_event(p->scheduler, process_event))
        scheduler_new_cpu_event(p->scheduler, process_event, p, 0, 0, IW_PROCESS_DELAY_NS);
    if (!has_event(p->scheduler, idle_event))
        scheduler_new_cpu_event(p->scheduler, idle_event, p, 0, 0, (uint64_t)p->set.idle_timeout_ms * 1000000ull);
}

static const scc_port_device_t iw_port_device_iw1 = {.name = "imagewriter", .tx_byte = port_tx_byte};
static const scc_port_device_t iw_port_device_iw2 = {.name = "imagewriter2", .tx_byte = port_tx_byte};

// Whether the printer takes data: selected, with paper
static bool printer_ready(const iw_printer_t *p) {
    return !p->set.panel_deselected && p->interp.st.selected && !p->set.paper_out;
}

// Tell the host whether the printer takes data: the ready line (DTR), or
// XON/XOFF when DIP switch 2-3 selects it.
static void update_ready(iw_printer_t *p) {
    if (p->set.connection != IW_CONN_SERIAL_A && p->set.connection != IW_CONN_SERIAL_B)
        return;
    unsigned ch = p->set.connection == IW_CONN_SERIAL_A ? 0 : 1;
    bool ready = printer_ready(p);
    if (p->set.icfg.dip2 & IW_DIP2_XONXOFF) {
        scc_port_device_ready(p->scc, ch, true); // DTR stays up
        if (!ready && !p->job.xoff_sent) {
            uint8_t dc3 = 0x13;
            p->job.xoff_sent = true;
            queue_reply(p, &dc3, 1);
        } else if (ready && p->job.xoff_sent) {
            uint8_t dc1 = 0x11;
            p->job.xoff_sent = false;
            queue_reply(p, &dc1, 1);
        }
        return;
    }
    scc_port_device_ready(p->scc, ch, ready);
}

// Plug into or out of the current connection's port.
static void port_attach(iw_printer_t *p, bool attach) {
    if (p->set.connection == IW_CONN_SERIAL_A || p->set.connection == IW_CONN_SERIAL_B) {
        unsigned ch = p->set.connection == IW_CONN_SERIAL_A ? 0 : 1;
        if (!p->scc)
            return;
        if (attach) {
            // The machine's wiring of the printer's DTR, if not wired already
            if (p->wiring[ch].wired && !p->wired_by_us[ch]) {
                scc_set_port_ready_line(p->scc, ch, p->wiring[ch].pin, p->wiring[ch].ready_level);
                p->wired_by_us[ch] = true;
            }
            scc_attach_port_device(p->scc, ch,
                                   p->set.icfg.model == IW_MODEL_IW1 ? &iw_port_device_iw1 : &iw_port_device_iw2, p);
            update_ready(p);
        } else {
            scc_attach_port_device(p->scc, ch, NULL, NULL);
            if (p->wired_by_us[ch]) {
                scc_unwire_port_ready_line(p->scc, ch);
                p->wired_by_us[ch] = false;
            }
        }
    } else if (p->set.connection == IW_CONN_LOCALTALK) {
        if (attach)
            g_localtalk = p;
        else if (g_localtalk == p)
            g_localtalk = NULL;
    }
}

iw_printer_t *iw_printer_localtalk(void) {
    return g_localtalk;
}

// Change the connection: the job in progress ends with the old one.
static bool set_connection(iw_printer_t *p, iw_connection_t conn) {
    if (conn == p->set.connection)
        return true;
    if (conn == IW_CONN_LOCALTALK && p->set.icfg.model != IW_MODEL_IW2)
        return false;
    iw_printer_end_job(p);
    port_attach(p, false);
    p->set.connection = (uint8_t)conn;
    // The LocalTalk Option card needs DIP switch 2-4 closed
    if (conn == IW_CONN_LOCALTALK)
        p->set.icfg.dip2 |= IW_DIP2_OPTION;
    else
        p->set.icfg.dip2 &= (uint8_t)~IW_DIP2_OPTION;
    port_attach(p, true);
    return true;
}

// --- Lifecycle ------------------------------------------------------------------

static void interp_setup(iw_printer_t *p) {
    iw_interp_hooks_t hooks = {.ctx = p, .page_done = on_page_done, .reply = on_reply, .select_changed = on_select};
    iw_interp_init(&p->interp, &p->set.icfg, &hooks);
}

// Read a length-prefixed blob from a checkpoint; *out malloc'd (NULL for 0).
static bool read_blob(checkpoint_t *cp, uint8_t **out, size_t *len, uint32_t max, const char *what) {
    uint32_t n = 0;
    *out = NULL;
    *len = 0;
    if (!checkpoint_read_count(cp, &n, max, what))
        return false;
    if (!n)
        return true;
    uint8_t *b = malloc(n);
    if (!b) {
        checkpoint_set_error(cp);
        return false;
    }
    system_read_checkpoint_data(cp, b, n, what);
    *out = b;
    *len = n;
    return !checkpoint_has_error(cp);
}

// Write a length-prefixed blob.
static void write_blob(checkpoint_t *cp, const uint8_t *data, size_t len, const char *what) {
    uint32_t n = (uint32_t)len;
    system_write_checkpoint_data(cp, &n, sizeof(n));
    if (n)
        system_write_checkpoint_data(cp, data, n, what);
}

// Restore everything iw_printer_checkpoint wrote.
static void restore(iw_printer_t *p, checkpoint_t *cp) {
    uint32_t version = 0;
    system_read_checkpoint_data(cp, &version, sizeof(version), "iwver");
    if (version != IW_CP_VERSION) {
        checkpoint_set_error(cp);
        return;
    }
    system_read_checkpoint_data(cp, &p->set, sizeof(p->set), "iwset");
    system_read_checkpoint_data(cp, &p->job, sizeof(p->job), "iwjob");
    if (p->set.paper >= N_PAPERS || p->set.connection >= IW_CONN_COUNT || p->set.icfg.model >= IW_MODEL_COUNT) {
        checkpoint_set_error(cp);
        default_settings(&p->set, IW_MODEL_IW2);
    }
    apply_paper(p);
    interp_setup(p);
    system_read_checkpoint_data(cp, &p->interp.st, sizeof(p->interp.st), "iwst");
    if (p->interp.st.n_marks > IW_LINE_MARKS || p->interp.st.pitch >= IW_PITCH_COUNT) {
        checkpoint_set_error(cp);
        iw_interp_power_on(&p->interp);
    }
    // The sheet's geometry follows the restored form length
    iw_interp_reconfigure(&p->interp);
    uint8_t *blob;
    size_t len;
    // The sheet in progress
    if (read_blob(cp, &blob, &len, 64u * 1024u * 1024u, "iwpage")) {
        if (!iw_page_deserialise(&p->interp.page, blob, len))
            checkpoint_set_error(cp);
        free(blob);
    }
    // The document so far
    if (read_blob(cp, &blob, &len, 256u * 1024u * 1024u, "iwpdf")) {
        if (len) {
            p->pdf = pdf_writer_deserialise(blob, len);
            if (!p->pdf)
                checkpoint_set_error(cp);
        }
        free(blob);
    }
    // Queued input and the capture
    if (read_blob(cp, &blob, &len, IW_INPUT_MAX, "iwin")) {
        byteq_append(&p->input, blob, len, IW_INPUT_MAX);
        free(blob);
    }
    if (read_blob(cp, &blob, &len, IW_CAPTURE_MAX, "iwcap")) {
        byteq_append(&p->capture, blob, len, IW_CAPTURE_MAX);
        free(blob);
    }
}

iw_printer_t *iw_printer_new(struct scheduler *scheduler, scc_t *scc, const iw_port_wiring_t wiring[2],
                             checkpoint_t *checkpoint) {
    iw_printer_t *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->scheduler = scheduler;
    p->scc = scc;
    if (wiring)
        memcpy(p->wiring, wiring, sizeof(p->wiring));
    if (scheduler) {
        scheduler_new_event_type(scheduler, "imagewriter", p, "process", &process_event);
        scheduler_new_event_type(scheduler, "imagewriter", p, "idle", &idle_event);
        scheduler_new_event_type(scheduler, "imagewriter", p, "reply", &reply_event);
    }
    if (checkpoint) {
        restore(p, checkpoint);
    } else {
        default_settings(&p->set, IW_MODEL_IW2);
        apply_paper(p);
        interp_setup(p);
        snprintf(p->job.last_outcome, sizeof(p->job.last_outcome), "%s", "");
    }
    // The cable is configuration: plug back in
    port_attach(p, true);
    update_status(p);

    p->object = object_new(&iw_printer_class, p, "imagewriter");
    if (p->object) {
        object_set_label(p->object, "ImageWriter");
        object_attach(machine_object(), p->object);
    }
    return p;
}

void iw_printer_delete(iw_printer_t *p) {
    if (!p)
        return;
    if (p->scheduler)
        scheduler_forget_source(p->scheduler, p);
    port_attach(p, false);
    if (p->object) {
        object_detach(p->object);
        object_delete(p->object);
    }
    iw_interp_free(&p->interp);
    pdf_writer_free(p->pdf);
    byteq_free(&p->input);
    byteq_free(&p->capture);
    free(p);
}

void iw_printer_checkpoint(iw_printer_t *p, checkpoint_t *cp) {
    if (!p || !cp)
        return;
    uint32_t version = IW_CP_VERSION;
    system_write_checkpoint_data(cp, &version, sizeof(version), "iwver");
    system_write_checkpoint_data(cp, &p->set, sizeof(p->set), "iwset");
    system_write_checkpoint_data(cp, &p->job, sizeof(p->job), "iwjob");
    system_write_checkpoint_data(cp, &p->interp.st, sizeof(p->interp.st), "iwst");
    uint8_t *blob = NULL;
    size_t len = 0;
    if (!iw_page_serialise(&p->interp.page, &blob, &len))
        checkpoint_set_error(cp);
    write_blob(cp, blob, len, "iwpage");
    free(blob);
    blob = NULL;
    len = 0;
    if (p->pdf && !pdf_writer_serialise(p->pdf, &blob, &len))
        checkpoint_set_error(cp);
    write_blob(cp, blob, len, "iwpdf");
    free(blob);
    write_blob(cp, byteq_data(&p->input), byteq_len(&p->input), "iwin");
    write_blob(cp, byteq_data(&p->capture), byteq_len(&p->capture), "iwcap");
}

// --- Object model --------------------------------------------------------------------

static iw_printer_t *printer_from(struct object *self) {
    return (iw_printer_t *)object_data(self);
}

// Settings that reshape the printer may change only between jobs
static value_t busy_error(const char *what) {
    return val_err("%s: the printer is printing; eject() first", what);
}

static DEF_GETTER(attr_model) {
    iw_printer_t *p = printer_from(self);
    return val_str(p->set.icfg.model == IW_MODEL_IW1 ? "imagewriter" : "imagewriter2");
}

static DEF_SETTER(attr_model_set) {
    iw_printer_t *p = printer_from(self);
    iw_model_t model;
    if (in.s && strcmp(in.s, "imagewriter") == 0)
        model = IW_MODEL_IW1;
    else if (in.s && strcmp(in.s, "imagewriter2") == 0)
        model = IW_MODEL_IW2;
    else {
        value_free(&in);
        return val_err("model: expected \"imagewriter\" or \"imagewriter2\"");
    }
    value_free(&in);
    if (p->job.job_active)
        return busy_error("model");
    if (model == p->set.icfg.model)
        return val_none();
    if (model == IW_MODEL_IW1 && p->set.connection == IW_CONN_LOCALTALK)
        return val_err("model: the LocalTalk Option card fits only the ImageWriter II");
    // A different printer on the same cable: unplug, swap, power on, plug in
    port_attach(p, false);
    p->set.icfg.model = model;
    p->set.icfg.color_ribbon = model == IW_MODEL_IW2;
    iw_interp_power_on(&p->interp);
    port_attach(p, true);
    update_status(p);
    return val_none();
}

static DEF_GETTER(attr_connection) {
    iw_printer_t *p = printer_from(self);
    return val_str(connection_names[p->set.connection]);
}

static DEF_SETTER(attr_connection_set) {
    iw_printer_t *p = printer_from(self);
    int conn = -1;
    for (int i = 0; i < IW_CONN_COUNT && in.s; i++)
        if (strcmp(in.s, connection_names[i]) == 0)
            conn = i;
    value_free(&in);
    if (conn < 0)
        return val_err("connection: expected none, serial-a, serial-b or localtalk");
    if (conn == IW_CONN_LOCALTALK && iw_printer_localtalk() && iw_printer_localtalk() != p)
        return val_err("connection: another printer is on LocalTalk");
    if (!set_connection(p, (iw_connection_t)conn))
        return val_err("connection: the LocalTalk Option card fits only the ImageWriter II");
    update_status(p);
    return val_none();
}

static DEF_GETTER(attr_dip1) {
    return val_uint(1, printer_from(self)->set.icfg.dip1);
}

static DEF_SETTER(attr_dip1_set) {
    iw_printer_t *p = printer_from(self);
    p->set.icfg.dip1 = (uint8_t)in.u;
    return val_none();
}

static DEF_GETTER(attr_dip2) {
    return val_uint(1, printer_from(self)->set.icfg.dip2);
}

static DEF_SETTER(attr_dip2_set) {
    iw_printer_t *p = printer_from(self);
    p->set.icfg.dip2 = (uint8_t)in.u;
    update_ready(p);
    return val_none();
}

static DEF_GETTER(attr_color_ribbon) {
    return val_bool(printer_from(self)->set.icfg.color_ribbon);
}

static DEF_SETTER(attr_color_ribbon_set) {
    iw_printer_t *p = printer_from(self);
    if (p->job.job_active)
        return busy_error("color_ribbon");
    if (in.b && p->set.icfg.model != IW_MODEL_IW2)
        return val_err("color_ribbon: the ImageWriter has no colour ribbon");
    p->set.icfg.color_ribbon = in.b;
    iw_interp_reconfigure(&p->interp);
    return val_none();
}

static DEF_GETTER(attr_sheet_feeder) {
    return val_bool(printer_from(self)->set.icfg.sheet_feeder);
}

static DEF_SETTER(attr_sheet_feeder_set) {
    printer_from(self)->set.icfg.sheet_feeder = in.b;
    return val_none();
}

static DEF_GETTER(attr_paper) {
    iw_printer_t *p = printer_from(self);
    return val_str(papers[p->set.paper].name);
}

static DEF_SETTER(attr_paper_set) {
    iw_printer_t *p = printer_from(self);
    int idx = -1;
    for (size_t i = 0; i < N_PAPERS && in.s; i++)
        if (strcmp(in.s, papers[i].name) == 0)
            idx = (int)i;
    value_free(&in);
    if (idx < 0)
        return val_err("paper: expected fanfold-letter, letter, a4, legal or fanfold-15in");
    if (p->job.job_active)
        return busy_error("paper");
    p->set.paper = (uint8_t)idx;
    apply_paper(p);
    iw_interp_reconfigure(&p->interp);
    return val_none();
}

static DEF_GETTER(attr_paper_mode) {
    return val_str(printer_from(self)->set.icfg.cut_sheet ? "cut-sheet" : "continuous");
}

static DEF_SETTER(attr_paper_mode_set) {
    iw_printer_t *p = printer_from(self);
    int mode = -1;
    if (in.s && strcmp(in.s, "continuous") == 0)
        mode = 0;
    else if (in.s && strcmp(in.s, "cut-sheet") == 0)
        mode = 1;
    value_free(&in);
    if (mode < 0)
        return val_err("paper_mode: expected continuous or cut-sheet");
    if (p->job.job_active)
        return busy_error("paper_mode");
    p->set.icfg.cut_sheet = mode == 1;
    iw_interp_reconfigure(&p->interp);
    return val_none();
}

static DEF_GETTER(attr_idle_timeout) {
    return val_uint(4, printer_from(self)->set.idle_timeout_ms);
}

static DEF_SETTER(attr_idle_timeout_set) {
    if (in.u < 10 || in.u > 3600000)
        return val_err("idle_timeout_ms: expected 10 .. 3600000");
    printer_from(self)->set.idle_timeout_ms = (uint32_t)in.u;
    return val_none();
}

static DEF_GETTER(attr_resolution) {
    return val_uint(2, printer_from(self)->set.icfg.dpi);
}

static DEF_SETTER(attr_resolution_set) {
    iw_printer_t *p = printer_from(self);
    if (in.u != 288 && in.u != 576)
        return val_err("resolution: expected 288 or 576");
    if (p->job.job_active)
        return busy_error("resolution");
    p->set.icfg.dpi = (uint16_t)in.u;
    iw_interp_reconfigure(&p->interp);
    return val_none();
}

static DEF_GETTER(attr_dot_shape) {
    return val_str(printer_from(self)->set.icfg.dot_shape == IW_DOT_SQUARE ? "square" : "disc");
}

static DEF_SETTER(attr_dot_shape_set) {
    iw_printer_t *p = printer_from(self);
    int shape = -1;
    if (in.s && strcmp(in.s, "disc") == 0)
        shape = IW_DOT_DISC;
    else if (in.s && strcmp(in.s, "square") == 0)
        shape = IW_DOT_SQUARE;
    value_free(&in);
    if (shape < 0)
        return val_err("dot_shape: expected disc or square");
    if (p->job.job_active)
        return busy_error("dot_shape");
    p->set.icfg.dot_shape = (uint8_t)shape;
    iw_interp_reconfigure(&p->interp);
    return val_none();
}

static DEF_GETTER(attr_tof_offset) {
    return val_uint(2, (uint64_t)printer_from(self)->set.icfg.tof_offset_144);
}

static DEF_SETTER(attr_tof_offset_set) {
    iw_printer_t *p = printer_from(self);
    if (in.u > 720)
        return val_err("tof_offset: expected 0 .. 720 (1/144 in)");
    if (p->job.job_active)
        return busy_error("tof_offset");
    // Takes effect at the next power-on: it says where the paper was loaded
    p->set.icfg.tof_offset_144 = (int32_t)in.u;
    return val_none();
}

// Pick up queued input once the printer is ready again.
static void resume_input(iw_printer_t *p) {
    if (paused(p) || !byteq_len(&p->input) || !p->scheduler)
        return;
    if (!has_event(p->scheduler, process_event))
        scheduler_new_cpu_event(p->scheduler, process_event, p, 0, 0, IW_PROCESS_DELAY_NS);
}

static DEF_GETTER(attr_selected) {
    return val_bool(iw_printer_selected(printer_from(self)));
}

// The front panel's SELECT button: off, the printer drops its ready line and
// stops; on, it is selected again (also after a DC3 from the host)
static DEF_SETTER(attr_selected_set) {
    iw_printer_t *p = printer_from(self);
    p->set.panel_deselected = !in.b;
    if (in.b)
        p->interp.st.selected = true;
    update_ready(p);
    update_status(p);
    resume_input(p);
    return val_none();
}

static DEF_GETTER(attr_paper_out) {
    return val_bool(printer_from(self)->set.paper_out);
}

static DEF_SETTER(attr_paper_out_set) {
    iw_printer_t *p = printer_from(self);
    p->set.paper_out = in.b;
    update_ready(p);
    update_status(p);
    resume_input(p);
    return val_none();
}

static DEF_GETTER(attr_capture) {
    return val_bool(printer_from(self)->set.capture);
}

static DEF_SETTER(attr_capture_set) {
    printer_from(self)->set.capture = in.b;
    return val_none();
}

static DEF_GETTER(attr_status) {
    return val_str(printer_from(self)->status);
}

static DEF_GETTER(attr_jobs) {
    return val_uint(4, printer_from(self)->job.jobs);
}

static DEF_GETTER(attr_pages) {
    return val_uint(4, printer_from(self)->job.pages);
}

static DEF_GETTER(attr_bytes) {
    return val_uint(8, printer_from(self)->job.bytes);
}

static DEF_GETTER(attr_busy) {
    return val_bool(printer_from(self)->job.job_active);
}

static DEF_GETTER(attr_last_job_pages) {
    return val_uint(4, printer_from(self)->job.last_job_pages);
}

static DEF_GETTER(attr_last_outcome) {
    return val_str(printer_from(self)->job.last_outcome);
}

static DEF_GETTER(attr_last_pdf_crc) {
    value_t v = val_uint(4, printer_from(self)->job.last_pdf_crc);
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_last_pdf_bytes) {
    return val_uint(4, printer_from(self)->job.last_pdf_len);
}

static DEF_GETTER(attr_pitch) {
    return val_str(iw_pitch_name(printer_from(self)->interp.st.pitch));
}

static DEF_GETTER(attr_line_spacing) {
    return val_uint(2, (uint64_t)printer_from(self)->interp.st.line_spacing);
}

static DEF_GETTER(attr_soft_switches) {
    const iw_printer_t *p = printer_from(self);
    value_t v = val_uint(2, (uint64_t)p->interp.st.soft_a << 8 | p->interp.st.soft_b);
    v.flags |= VAL_HEX;
    return v;
}

static DEF_METHOD(method_eject) {
    iw_printer_end_job(printer_from(self));
    return val_none();
}

static DEF_METHOD(method_reset) {
    iw_printer_t *p = printer_from(self);
    iw_printer_end_job(p);
    iw_interp_power_on(&p->interp);
    update_ready(p);
    update_status(p);
    return val_none();
}

static DEF_METHOD(method_feed) {
    iw_printer_t *p = printer_from(self);
    if (argv[0].kind == V_BYTES) {
        iw_printer_feed(p, argv[0].bytes.p, argv[0].bytes.n);
        return val_uint(4, argv[0].bytes.n);
    }
    if (argv[0].kind == V_STRING) {
        size_t n = argv[0].s ? strlen(argv[0].s) : 0;
        iw_printer_feed(p, (const uint8_t *)argv[0].s, n);
        return val_uint(4, n);
    }
    if (argv[0].kind == V_INT || argv[0].kind == V_UINT) {
        uint8_t b = (uint8_t)(argv[0].kind == V_INT ? (uint64_t)argv[0].i : argv[0].u);
        iw_printer_feed(p, &b, 1);
        return val_uint(4, 1);
    }
    return val_err("feed: expected a string, bytes or a byte value");
}

static DEF_METHOD(method_feed_file) {
    iw_printer_t *p = printer_from(self);
    const char *path = argv[0].s;
    vfs_file_t *f = NULL;
    const vfs_backend_t *be = NULL;
    if (vfs_open(path, &f, &be) != 0 || !f)
        return val_err("feed_file: cannot open '%s'", path);
    uint8_t buf[8192];
    uint64_t off = 0;
    for (;;) {
        size_t got = 0;
        if (be->read(f, off, buf, sizeof(buf), &got) != 0) {
            be->close(f);
            return val_err("feed_file: read error in '%s'", path);
        }
        if (!got)
            break;
        iw_printer_feed(p, buf, got);
        off += got;
    }
    be->close(f);
    return val_uint(8, off);
}

static const arg_decl_t feed_args[] = {
    {.name = "data", .kind = V_NONE, .validation_flags = OBJ_ARG_POLY, .doc = "Bytes, a string, or one byte value"},
};

static const arg_decl_t feed_file_args[] = {
    ARG_PATH("path", "File holding a raw ImageWriter byte stream"),
};

static const member_t iw_printer_members[] = {
    {.kind = M_ATTR,
     .name = "model",
     .doc = "Which printer: imagewriter (the original) or imagewriter2",
     .attr = {.type = V_STRING, .get = attr_model, .set = attr_model_set}},
    {.kind = M_ATTR,
     .name = "connection",
     .doc = "Where it is plugged in: none, serial-a, serial-b, or localtalk (the ImageWriter II's LocalTalk "
            "Option card, printing over AppleTalk)", .examples = EXAMPLES("machine.imagewriter.connection = \"serial-b\""),
     .attr = {.type = V_STRING, .get = attr_connection, .set = attr_connection_set}},
    {.kind = M_ATTR,
     .name = "status",
     .doc = "What the printer is doing, as the UI shows it",
     .attr = {.type = V_STRING, .get = attr_status}},
    {.kind = M_ATTR,
     .name = "busy",
     .doc = "True while a job is in progress",
     .attr = {.type = V_BOOL, .get = attr_busy}},
    {.kind = M_ATTR,
     .name = "dip1",
     .flags = M_CAT_ADVANCED,
     .doc = "DIP switch bank 1, bit n-1 = switch 1-n closed (language, form length, perforation skip, pitch, "
            "LF after CR); read at power-on", .attr = {.type = V_UINT, .width = 1, .presentation_flags = VAL_HEX, .get = attr_dip1, .set = attr_dip1_set}},
    {.kind = M_ATTR,
     .name = "dip2",
     .flags = M_CAT_ADVANCED,
     .doc = "DIP switch bank 2, bit n-1 = switch 2-n closed (baud rate, XON/XOFF, option card)",
     .attr = {.type = V_UINT, .width = 1, .presentation_flags = VAL_HEX, .get = attr_dip2, .set = attr_dip2_set}},
    {.kind = M_ATTR,
     .name = "color_ribbon",
     .doc = "A colour ribbon is fitted (ImageWriter II)",
     .attr = {.type = V_BOOL, .get = attr_color_ribbon, .set = attr_color_ribbon_set}},
    {.kind = M_ATTR,
     .name = "sheet_feeder",
     .doc = "A SheetFeeder is fitted (reported by ESC ? and the AppleTalk status)",
     .attr = {.type = V_BOOL, .get = attr_sheet_feeder, .set = attr_sheet_feeder_set}},
    {.kind = M_ATTR,
     .name = "paper",
     .doc = "Paper loaded: fanfold-letter, letter, a4, legal or fanfold-15in",
     .attr = {.type = V_STRING, .get = attr_paper, .set = attr_paper_set}},
    {.kind = M_ATTR,
     .name = "paper_mode",
     .doc = "continuous (fanfold) or cut-sheet (each form feed ejects the sheet)",
     .attr = {.type = V_STRING, .get = attr_paper_mode, .set = attr_paper_mode_set}},
    {.kind = M_ATTR,
     .name = "idle_timeout_ms",
     .flags = M_CAT_ADVANCED,
     .doc = "Guest time with no input after which a serial job ends",
     .attr = {.type = V_UINT, .get = attr_idle_timeout, .set = attr_idle_timeout_set}},
    {.kind = M_ATTR,
     .name = "resolution",
     .flags = M_CAT_ADVANCED,
     .doc = "Raster resolution of the PDF's pages: 288 or 576 dpi",
     .attr = {.type = V_UINT, .get = attr_resolution, .set = attr_resolution_set}},
    {.kind = M_ATTR,
     .name = "dot_shape",
     .flags = M_CAT_ADVANCED,
     .doc = "disc (like the wires) or square (crisp, for diagnosis)",
     .attr = {.type = V_STRING, .get = attr_dot_shape, .set = attr_dot_shape_set}},
    {.kind = M_ATTR,
     .name = "tof_offset",
     .flags = M_CAT_ADVANCED,
     .doc = "How far below the sheet's top edge the print line is at top of form, in 1/144 in (default 80, "
            "what the drivers assume; the manual recommends 72); applies from the next reset", .attr = {.type = V_UINT, .get = attr_tof_offset, .set = attr_tof_offset_set}},
    {.kind = M_ATTR,
     .name = "selected",
     .doc = "The front panel's SELECT light: deselected, the printer drops its ready line and stops printing "
            "(what it already received waits in its buffer)", .attr = {.type = V_BOOL, .get = attr_selected, .set = attr_selected_set}},
    {.kind = M_ATTR,
     .name = "paper_out",
     .doc = "Simulate running out of paper",
     .attr = {.type = V_BOOL, .get = attr_paper_out, .set = attr_paper_out_set}},
    {.kind = M_ATTR,
     .name = "capture",
     .flags = M_CAT_ADVANCED,
     .doc = "Also hand each job's raw input to the platform (<print-dir>/<printer>-<job>.iw in headless)",
     .attr = {.type = V_BOOL, .get = attr_capture, .set = attr_capture_set}},
    {.kind = M_ATTR, .name = "jobs", .doc = "Documents printed", .attr = {.type = V_UINT, .get = attr_jobs}},
    {.kind = M_ATTR,
     .name = "pages",
     .doc = "Pages printed, all documents",
     .attr = {.type = V_UINT, .get = attr_pages}},
    {.kind = M_ATTR,
     .name = "bytes",
     .flags = M_CAT_ADVANCED,
     .doc = "Bytes received",
     .attr = {.type = V_UINT, .get = attr_bytes}},
    {.kind = M_ATTR,
     .name = "last_job_pages",
     .doc = "Pages in the last document",
     .attr = {.type = V_UINT, .get = attr_last_job_pages}},
    {.kind = M_ATTR,
     .name = "last_outcome",
     .doc = "How the last job ended: \"\" before any, \"ok\", \"ok (placeholder font)\" or \"failed: <why>\"",
     .attr = {.type = V_STRING, .get = attr_last_outcome}},
    {.kind = M_ATTR,
     .name = "last_pdf_crc",
     .flags = M_CAT_ADVANCED,
     .doc = "CRC-32 of the last document's PDF (byte-exact goldens)",
     .attr = {.type = V_UINT, .get = attr_last_pdf_crc}},
    {.kind = M_ATTR,
     .name = "last_pdf_bytes",
     .flags = M_CAT_ADVANCED,
     .doc = "Size of the last document's PDF",
     .attr = {.type = V_UINT, .get = attr_last_pdf_bytes}},
    {.kind = M_ATTR,
     .name = "pitch",
     .flags = M_CAT_ADVANCED,
     .doc = "The character pitch selected now",
     .attr = {.type = V_STRING, .get = attr_pitch}},
    {.kind = M_ATTR,
     .name = "line_spacing",
     .flags = M_CAT_ADVANCED,
     .doc = "The line feed distance now, in 1/144 in",
     .attr = {.type = V_UINT, .get = attr_line_spacing}},
    {.kind = M_ATTR,
     .name = "soft_switches",
     .flags = M_CAT_ADVANCED,
     .doc = "Software switch registers A (high byte) and B (low byte), bit set = switch closed",
     .attr = {.type = V_UINT, .get = attr_soft_switches}},
    {.kind = M_METHOD,
     .name = "eject",
     .doc = "End the job now: its document goes out",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = method_eject, .ui_flags = MM_MUTATE}},
    {.kind = M_METHOD,
     .name = "reset",
     .doc = "Switch the printer off and on: settings from the DIP switches, custom characters gone",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = method_reset, .ui_flags = MM_MUTATE}},
    {.kind = M_METHOD,
     .name = "feed",
     .flags = M_CAT_ADVANCED,
     .doc = "Give the printer bytes directly, as if they came down the cable",
     .method = {.args = feed_args, .nargs = 1, .result = V_UINT, .fn = method_feed, .ui_flags = MM_MUTATE}},
    {.kind = M_METHOD,
     .name = "feed_file",
     .flags = M_CAT_ADVANCED,
     .doc = "Give the printer a file's bytes (a captured job)",
     .method = {.args = feed_file_args, .nargs = 1, .result = V_UINT, .fn = method_feed_file, .ui_flags = MM_MUTATE}},
};

static const class_desc_t iw_printer_class = {
    .name = "imagewriter",
    .doc = "The virtual ImageWriter dot-matrix printer; each job becomes a PDF",
    .members = iw_printer_members,
    .n_members = sizeof(iw_printer_members) / sizeof(iw_printer_members[0]),
};
