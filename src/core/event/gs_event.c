// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gs_event.c -- see gs_event.h.  The weak defaults: no delivery, no client.

#include "gs_event.h"

#include <stdarg.h>
#include <stdio.h>

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
