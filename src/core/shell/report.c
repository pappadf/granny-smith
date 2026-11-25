// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// report.c -- see report.h.

#include "report.h"

#include "value.h"
#include "job/job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// `s`, cut to `max` bytes (0: whole), as a string value.
static value_t str_cut(const char *s, size_t max) {
    if (!max || strlen(s) <= max)
        return val_str(s);
    char *cut = strndup(s, max);
    value_t v = val_str(cut ? cut : "");
    free(cut);
    return v;
}

// The fields of an `error` annotation; `max` bounds each string (the
// reduced form, which says `truncated`), 0 for the full one.
static value_t error_fields(const char *file, int line, const char *msg, const char *text, size_t max) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "file", str_cut(file, max ? 256 : 0));
    val_map_put(b, "line", val_int(line));
    val_map_put(b, "message", str_cut(msg, max));
    // The stderr text split at newlines, the empty last element dropped.
    value_t *items = NULL;
    size_t len = 0, cap = 0;
    for (const char *p = text; *p;) {
        size_t n = strcspn(p, "\n");
        char *ln = strndup(p, n);
        val_list_push(&items, &len, &cap, str_cut(ln ? ln : "", max));
        free(ln);
        p += n;
        if (*p == '\n')
            p++;
    }
    val_map_put(b, "lines", val_list(items, len));
    if (max)
        val_map_put(b, "truncated", val_bool(true));
    return val_map_finish(b);
}

void script_report_error(const char *file, int line, const char *msg, const char *text) {
    // Inside a job: once, as an `error` annotation at this point of the
    // job's record stream (clients print or render it).  stderr only when no
    // record can carry it whole: outside a job, past the output cut, or when
    // only the shortened form fits (that record says `truncated`).
    file = file ? file : "";
    msg = msg ? msg : "";
    value_t full = error_fields(file, line, msg, text, 0);
    value_t reduced = error_fields(file, line, msg, text, 512);
    bool shortened = false;
    if (!job_annotate("error", &full, &reduced, &shortened) || shortened)
        fputs(text, stderr);
    value_free(&full);
    value_free(&reduced);
}
