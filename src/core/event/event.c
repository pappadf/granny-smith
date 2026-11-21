// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// event.c -- see event.h.  The weak defaults: no delivery, no client.

#include "event.h"

#include "json_escape.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((weak)) void event_emit(event_kind_t kind, const char *json) {
    (void)kind;
    (void)json;
}

__attribute__((weak)) uint32_t platform_current_client(void) {
    return 0;
}

static int s_held;

void event_hold(int held) {
    s_held = held;
}

void event_emitf(event_kind_t kind, const char *fmt, ...) {
    if (s_held && kind != GS_EVENT_LOG)
        return;
    char buf[GS_EVENT_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof buf)
        return;
    event_emit(kind, buf);
}

void event_emit_text(event_kind_t kind, const char *event, const char *field, const char *text) {
    if (s_held && kind != GS_EVENT_LOG)
        return;
    if (!text)
        text = "";
    size_t n = strlen(text);
    // Worst case every byte escapes to \uXXXX
    size_t cap = strlen(event) + strlen(field) + JSON_ESCAPE_MAX * n + 32;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return;
    size_t o = (size_t)snprintf(buf, cap, "{\"event\":\"%s\",\"%s\":\"", event, field);
    for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
        size_t k = json_escape_char(*c, buf + o);
        if (k)
            o += k;
        else
            buf[o++] = (char)*c;
    }
    buf[o++] = '"';
    buf[o++] = '}';
    buf[o] = '\0';
    event_emit(kind, buf);
    free(buf);
}
