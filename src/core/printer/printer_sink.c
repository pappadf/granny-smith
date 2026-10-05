// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// printer_sink.c
// Weak defaults for the platform's printer sinks (printer_sink.h).

#include "printer_sink.h"

#include "log.h"

LOG_USE_CATEGORY_NAME("imagewriter");

// Documents delivered since the process started
static uint32_t g_documents;

// Fallback when no platform sink is linked: the document is lost, loudly.
__attribute__((weak)) void printer_sink_document(const printer_document_t *doc) {
    LOG(1, "%s: job %u '%s' (%u pages, %zu bytes) dropped: no document sink on this platform", doc->printer,
        (unsigned)doc->job_id, doc->title, (unsigned)doc->pages, doc->pdf_len);
}

// Fallback for a capture: the same.
__attribute__((weak)) void printer_sink_capture(const printer_capture_t *cap) {
    LOG(1, "%s: job %u input (%zu bytes) dropped: no capture sink on this platform", cap->slug, (unsigned)cap->job_id,
        cap->len);
}

// Fallback for a status change: nobody is listening.
__attribute__((weak)) void printer_sink_status(const char *printer, const char *status) {
    (void)printer;
    (void)status;
}

uint32_t printer_sink_documents(void) {
    return g_documents;
}

void printer_sink_deliver(const printer_document_t *doc) {
    g_documents++;
    printer_sink_document(doc);
}
