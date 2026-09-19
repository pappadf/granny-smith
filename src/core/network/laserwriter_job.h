// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// laserwriter_job.h
// The PostScript side of the emulated LaserWriter: one interpreter job per
// EOF-delimited PAP job, run by EfterScript's platen library.  The PAP layer
// (appletalk_printer.c) feeds it the workstation's bytes and pulls back the
// program's output; the platform supplies the sink that keeps the document.
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
#define LASERWRITER_PRODUCT  "LaserWriter II NT"
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

// === Operations ===

// True when the interpreter is linked in (PLATEN=1).
bool laserwriter_job_available(void);

// Starts the job for PAP job `job_id`: a fresh interpreter seeded with the
// identity and prelude.  Returns false (with the reason logged) when the
// library refuses the configuration, which a caller reports as busy.
bool laserwriter_job_begin(uint32_t job_id);

// True between begin and finish/abort.
bool laserwriter_job_active(void);

// Feeds `len` bytes of the program as they arrived from the wire, in order;
// the interpreter runs as far as they allow.  Output produced by the call
// waits for laserwriter_job_read_output.
void laserwriter_job_feed(const uint8_t *data, size_t len);

// Copies pending program output (replies and error reports, in the order
// each channel produced them) into `buf`, at most `cap`; returns the count,
// 0 when nothing is pending.  Call until it returns 0.
size_t laserwriter_job_read_output(uint8_t *buf, size_t cap);

// End of data: runs the program to completion, hands the document to the
// platform sink, and frees the job.  Output from the completion is left for
// laserwriter_job_read_output.
void laserwriter_job_finish(void);

// Frees the job without finishing (the connection went away mid-job).
void laserwriter_job_abort(void);

// Writes the PAP status text for the current state into `out`: idle, busy
// with the job name, or printing with the page count.
void laserwriter_job_status(char *out, size_t cap);

// Pages shown so far by the running job.
uint32_t laserwriter_job_pages(void);

// Observability for the object model: documents produced, pages and outcome
// of the last finished job ("" before any job; "ok"; "error: <name> in
// <command>"; "budget").
uint32_t laserwriter_job_documents(void);
uint32_t laserwriter_job_last_pages(void);
const char *laserwriter_job_last_outcome(void);

// Platform sink for a finished document.  The weak default in
// laserwriter_job.c logs and drops it; headless_main.c writes
// <print-dir>/<job>-<title>.pdf, em_main.c keeps the bytes for the browser.
void laserwriter_sink_document(const laserwriter_document_t *doc);

#endif // LASERWRITER_JOB_H
