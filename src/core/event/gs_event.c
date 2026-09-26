// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gs_event.c -- see gs_event.h.  The weak defaults: no delivery, no client.

#include "gs_event.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((weak)) void gs_event_emit(gs_event_kind_t kind, const char *json) {
    (void)kind;
    (void)json;
}

__attribute__((weak)) uint32_t gs_current_client(void) {
    return 0;
}

void gs_event_emitf(gs_event_kind_t kind, const char *fmt, ...) {
    char buf[GS_EVENT_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof buf)
        return;
    gs_event_emit(kind, buf);
}

void gs_event_emit_text(gs_event_kind_t kind, const char *event, const char *field, const char *text) {
    if (!text)
        text = "";
    size_t n = strlen(text);
    // Worst case every byte escapes to \uXXXX (6 bytes).
    size_t cap = strlen(event) + strlen(field) + 6 * n + 32;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return;
    size_t o = (size_t)snprintf(buf, cap, "{\"event\":\"%s\",\"%s\":\"", event, field);
    for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
        if (*c == '"' || *c == '\\') {
            buf[o++] = '\\';
            buf[o++] = (char)*c;
        } else if (*c == '\n') {
            buf[o++] = '\\';
            buf[o++] = 'n';
        } else if (*c < 0x20) {
            o += (size_t)snprintf(buf + o, 8, "\\u%04x", *c);
        } else {
            buf[o++] = (char)*c;
        }
    }
    buf[o++] = '"';
    buf[o++] = '}';
    buf[o] = '\0';
    gs_event_emit(kind, buf);
    free(buf);
}
