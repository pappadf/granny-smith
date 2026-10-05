// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// printer_sink.h
// Where a finished print job goes: the platform's document sink, shared by
// every emulated printer that produces a PDF in the core (the ImageWriter
// today).  The core never names a file or a download; it hands the platform
// the bytes and the job's identity, and the platform keeps them -- headless
// writes <print-dir>/<printer>-<job>-<title>.pdf, the browser stages the
// bytes to the page, which opens them in the print viewer.

#ifndef PRINTER_SINK_H
#define PRINTER_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Longest title a job carries (bytes, without the terminator).
#define PRINTER_TITLE_MAX 63

// A finished document.  The bytes are valid only for the duration of the
// sink call; the sink copies what it keeps.
typedef struct {
    const char *printer; // display name: "ImageWriter II", ...
    const char *slug; // file-name form of the printer: "imagewriter2", ...
    uint32_t job_id; // the printer's own job counter
    const char *title; // "" when unknown
    const uint8_t *pdf; // the document
    size_t pdf_len;
    uint32_t pages; // pages in the document
    bool ok; // the job finished normally
    const char *detail; // what went wrong, "" when ok
} printer_document_t;

// A job's raw input, as the printer received it (the printer's `capture`
// switch): the byte stream a corpus is made of.
typedef struct {
    const char *slug; // as in printer_document_t
    uint32_t job_id;
    const char *ext; // file extension without the dot: "iw"
    const uint8_t *data;
    size_t len;
} printer_capture_t;

// Platform sink for a document.  The weak default in printer_sink.c logs
// and drops it.
void printer_sink_document(const printer_document_t *doc);

// Platform sink for a capture.  The weak default logs and drops it.
void printer_sink_capture(const printer_capture_t *cap);

// A printer's status changed: `status` is the `key: value; ...` text the
// UI shows.  The weak default does nothing; the browser forwards it to the
// page as a printer_status event.
void printer_sink_status(const char *printer, const char *status);

// The documents handed to printer_sink_document since the process started
// (for tests that run without a platform sink).
uint32_t printer_sink_documents(void);

// Hands `doc` to the platform sink and counts it.
void printer_sink_deliver(const printer_document_t *doc);

#endif // PRINTER_SINK_H
