// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// util.c
// Shared helpers: entry names made safe as path components.

#include "internal.h"

// ============================================================================
// Entry names (see internal.h / peeler.h)
// ============================================================================

void peel_append_segment(char *dst, size_t cap, size_t *pos, const uint8_t *name, size_t n) {
    if (cap == 0)
        return;
    size_t p = *pos < cap ? *pos : cap - 1;
    if (p > 0 && p < cap - 1)
        dst[p++] = '/';
    bool dots_only = n > 0;
    for (size_t i = 0; i < n; i++)
        if (name[i] != '.')
            dots_only = false;
    if (n == 0 || dots_only) {
        if (p < cap - 1)
            dst[p++] = '_';
    }
    for (size_t i = 0; i < n && p < cap - 1; i++)
        dst[p++] = (name[i] == '/' || name[i] == '\0') ? ':' : (char)name[i];
    dst[p] = '\0';
    *pos = p;
}

bool peel_path_is_confined(const char *path) {
    if (!path || !path[0] || path[0] == '/')
        return false;
    const char *c = path;
    for (;;) {
        const char *slash = strchr(c, '/');
        size_t len = slash ? (size_t)(slash - c) : strlen(c);
        if (len == 0 || (len == 1 && c[0] == '.') || (len == 2 && c[0] == '.' && c[1] == '.'))
            return false;
        if (!slash)
            return true;
        c = slash + 1;
    }
}
