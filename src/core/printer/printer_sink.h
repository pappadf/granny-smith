// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// printer_sink.h
// Where a finished print job goes: the platform's document sink, shared by
// every emulated printer -- the ImageWriter, which rasterises in the core,
// and the LaserWriter, whose interpreter hands back a PDF (headless; in the
// browser its worker posts the PDF to the page instead).  The core never
// names a file or a download; it hands the platform the bytes and the job's
// identity, and the platform keeps them -- headless writes
// <print-dir>/<printer_file_name>, the browser stages the bytes to the
// page, which opens them in the print viewer.  printer_file_name is the one
// rule both platforms name the file by.

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
    const char *printer; // display name: "ImageWriter II", "LaserWriter", ...
    const char *slug; // file-name form of the printer: "imagewriter2"; NULL: no prefix (the LaserWriter)
    uint32_t job_id; // the printer's own job counter
    const char *title; // "" when unknown
    const uint8_t *pdf; // the document
    size_t pdf_len;
    uint32_t pages; // pages in the document
    bool ok; // the job finished normally
    const char *detail; // what went wrong, "" when ok: "error: <name> in <offending>", "execution budget spent"
} printer_document_t;

// A job's raw input, as the printer received it (the printer's `capture`
// switch): the byte stream a corpus is made of.
typedef struct {
    const char *printer; // as in printer_document_t
    const char *slug; // as in printer_document_t
    uint32_t job_id;
    const char *ext; // file extension without the dot: "iw", "ps"
    const uint8_t *data;
    size_t len;
    bool complete; // the job ended normally; false for one cut off
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

// `title` reduced to file-name-safe characters: one '_' per run of anything
// outside [A-Za-z0-9.-], none leading, at most PRINTER_TITLE_MAX bytes;
// "untitled" when nothing is left.  Returns the length written.
size_t printer_safe_title(char *out, size_t cap, const char *title);

// The file name of a job's output: `<slug>-<job>-<safe title>.<ext>`, or
// `<slug>-<job>.<ext>` when `title` is NULL (a capture); without the
// `<slug>-` when `slug` is NULL or "".  The job number is five digits.
// Returns snprintf's count.
int printer_file_name(char *out, size_t cap, const char *slug, uint32_t job_id, const char *title, const char *ext);

#endif // PRINTER_SINK_H
