// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// laserwriter_job.h
// The PostScript side of the emulated LaserWriter: one interpreter job per
// EOF-delimited PAP job, run by EfterScript's platen library behind a
// laserwriter_transport (in this process for headless, in a Web Worker for
// the browser).  Everything is asynchronous: the PAP layer
// (appletalk_printer.c) issues begin / feed / finish and hears back through
// a listener; the platform supplies the sink that keeps the document.
// Built only with PLATEN=1 (GS_PLATEN); without it every entry is a stub and
// laserwriter_job_available() reports false.

#ifndef LASERWRITER_JOB_H
#define LASERWRITER_JOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Constants ===

// The device the interpreter presents.  Seeded as statusdict identity
// entries, which platen also mirrors into systemdict, so the two agree;
// the prelude (laserwriter_prelude.ps) defines everything else.
//
// The product deliberately does NOT begin with "LaserWriter".  Apple's
// drivers key their native-code paths off exactly that prefix: the
// AppleDict prologue computes
//     LW = statusdict/product get (LaserWriter) anchorsearch ...
// and a non-zero LW makes the driver download 68000 code for the
// printer's own processor through `eexec` + `cexec` (System 7.1's
// LaserWriter 7.1.2 does this for its smoothing procedures).  We are a
// software interpreter with no such processor, so we answer LW = 0 and
// the driver sends the generic PostScript path instead.
#define LASERWRITER_PRODUCT  "EfterScript LaserWriter"
#define LASERWRITER_VERSION  "47.0"
#define LASERWRITER_REVISION "0"

// Longest job name kept from the driver's %%Title: comment.
#define LASERWRITER_TITLE_MAX 63

// === Type Definitions ===

// A finished job handed to the platform sink.  The bytes are valid only for
// the duration of the call; the sink copies what it keeps.
typedef struct {
    uint32_t job_id; // PAP job counter value for the job
    const char *title; // job name from %%Title:, "" when absent
    const uint8_t *pdf; // the document, complete whatever the outcome
    size_t pdf_len;
    uint32_t pages; // pages in the document
    bool ok; // the program ran to the end of its data
    const char *error_name; // PostScript error name, "" when ok
    const char *offending; // offending command, "" when ok
    bool budget_exceeded; // stopped by the execution budget
} laserwriter_document_t;

// What the bridge tells the PAP layer, always from a scheduler event or a
// poll — never from inside a begin / feed / finish call.
typedef enum {
    LASERWRITER_EVENT_OPENED = 0, // the job takes data: the first SendData may go out
    LASERWRITER_EVENT_FED, // the last feed is acknowledged; its output is readable
    LASERWRITER_EVENT_FINISHED, // the job is over: output readable, document counted
    LASERWRITER_EVENT_FAILED // the job is gone (open refused, feed or finish failed,
                             //   interpreter lost); `detail` says why
} laserwriter_event_t;

typedef void (*laserwriter_listener_t)(laserwriter_event_t event, const char *detail, void *ctx);

// === Operations ===

// True when the interpreter is linked in (PLATEN=1).
bool laserwriter_job_available(void);

// Installs the PAP layer's listener (once).
void laserwriter_job_set_listener(laserwriter_listener_t fn, void *ctx);

// Register the bridge's and the transport's timers with the stack's
// scheduler.  Called from atalk_printer_register each time the stack comes up,
// so a checkpoint restore finds them (atalk_timer_t).
void laserwriter_job_init(void);

// Starts the job for PAP job `job_id`: a fresh interpreter seeded with the
// identity and prelude, opened through the transport.  Returns false when
// the request could not be issued; otherwise OPENED or FAILED follows.
// A job still held is abandoned first.
bool laserwriter_job_begin(uint32_t job_id);

// True between begin and FINISHED / FAILED / abort.
bool laserwriter_job_active(void);

// True once OPENED arrived and no feed or finish is outstanding: the PAP
// layer may read the next SendData's worth of data.
bool laserwriter_job_ready(void);

// True while a feed or a finish awaits its acknowledgement.  A query's
// reply arrives with that acknowledgement, so the PAP layer answers no
// read credit with a status line meanwhile (Inside AppleTalk 2e ch. 10:
// the read-driven model — the credit waits for real data).
bool laserwriter_job_feed_pending(void);

// Feeds `len` bytes of the program (at most one flow quantum) that answered
// SendData `sequence`.  FED follows; output produced by the feed waits for
// laserwriter_job_read_output.  False when not issued (not ready).
bool laserwriter_job_feed(uint32_t sequence, const uint8_t *data, size_t len);

// Copies pending program output (replies and error reports, in the order
// each channel produced them) into `buf`, at most `cap`; returns the count,
// 0 when nothing is pending.  Call until it returns 0.
size_t laserwriter_job_read_output(uint8_t *buf, size_t cap);

// End of data: the program runs to completion; FINISHED follows, after the
// document went to the platform sink and the counters moved.  False when
// not issued (not ready).
bool laserwriter_job_finish(void);

// Frees the job without finishing (the connection went away mid-job).
void laserwriter_job_abort(void);

// Writes the PAP status text for the current state into `out`: idle,
// starting up (open outstanding), busy with the job name, or printing with
// the page count.
void laserwriter_job_status(char *out, size_t cap);

// Pages shown so far by the running job (as of the last acknowledgement).
uint32_t laserwriter_job_pages(void);

// Observability for the object model: documents produced, pages and outcome
// of the last finished job ("" before any job; "ok"; "error: <name> in
// <command>"; "budget"; "failed: <reason>").
uint32_t laserwriter_job_documents(void);
uint32_t laserwriter_job_last_pages(void);
const char *laserwriter_job_last_outcome(void);

// Platform sink for a finished document.  The weak default in
// laserwriter_job.c logs and drops it; headless_main.c writes
// <print-dir>/<job>-<title>.pdf.  The browser never sees the bytes here
// (its worker posts the PDF to the page), so the sink is not called there.
void laserwriter_sink_document(const laserwriter_document_t *doc);

// A job's PostScript as the workstation sent it (appletalk.printer.capture),
// handed over when the job ends.  The bytes are valid only for the duration
// of the call.
typedef struct {
    uint32_t job_id; // PAP job counter value, as in laserwriter_document_t
    const uint8_t *ps;
    size_t ps_len;
    bool complete; // the job ended normally; false for one cut off
} laserwriter_capture_t;

// Platform sink for a capture.  The weak default in laserwriter_job.c logs
// and drops it; headless_main.c writes <print-dir>/<job>.ps beside the PDF;
// the browser downloads it.  The core printer holds no file of its own.
void laserwriter_sink_capture(const laserwriter_capture_t *cap);

#endif // LASERWRITER_JOB_H
