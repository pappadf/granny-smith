// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// printer_sink.c
// Weak defaults for the platform's printer sinks, and the one file-name rule
// (printer_sink.h).

#include "printer_sink.h"

#include "log.h"

#include <stdio.h>

LOG_USE_CATEGORY_NAME("printer");

// Fallback when no platform sink is linked: the document is lost, loudly.
__attribute__((weak)) void printer_sink_document(const printer_document_t *doc) {
    LOG(1, "%s: job %u '%s' (%u pages, %zu bytes) dropped: no document sink on this platform", doc->printer,
        (unsigned)doc->job_id, doc->title, (unsigned)doc->pages, doc->pdf_len);
}

// Fallback for a capture: the same.
__attribute__((weak)) void printer_sink_capture(const printer_capture_t *cap) {
    LOG(1, "%s: job %u input (%zu bytes) dropped: no capture sink on this platform", cap->printer,
        (unsigned)cap->job_id, cap->len);
}

// Fallback for a status change: nobody is listening.
__attribute__((weak)) void printer_sink_status(const char *printer, const char *status) {
    (void)printer;
    (void)status;
}

size_t printer_safe_title(char *out, size_t cap, const char *title) {
    if (!out || cap == 0)
        return 0;
    size_t max = cap - 1 < PRINTER_TITLE_MAX ? cap - 1 : PRINTER_TITLE_MAX;
    size_t n = 0;
    bool pending_sep = false;
    for (const char *p = title ? title : ""; *p && n < max; p++) {
        unsigned char c = (unsigned char)*p;
        bool keep = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '-';
        if (!keep) {
            pending_sep = true; // a run of anything else becomes one '_'
            continue;
        }
        // The run's '_' goes in only between kept characters, never leading
        if (pending_sep && n > 0)
            out[n++] = '_';
        pending_sep = false;
        if (n < max)
            out[n++] = (char)c;
    }
    out[n] = '\0';
    if (n == 0)
        n = (size_t)snprintf(out, cap, "untitled");
    return n < cap ? n : cap - 1;
}

int printer_file_name(char *out, size_t cap, const char *slug, uint32_t job_id, const char *title, const char *ext) {
    bool prefix = slug && *slug;
    if (!title)
        return snprintf(out, cap, "%s%s%05u.%s", prefix ? slug : "", prefix ? "-" : "", (unsigned)job_id, ext);
    char safe[PRINTER_TITLE_MAX + 1];
    printer_safe_title(safe, sizeof safe, title);
    return snprintf(out, cap, "%s%s%05u-%s.%s", prefix ? slug : "", prefix ? "-" : "", (unsigned)job_id, safe, ext);
}
