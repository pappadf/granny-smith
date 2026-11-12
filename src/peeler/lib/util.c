// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// util.c
// Shared utility implementations: growable output buffers.

#include "internal.h"

// ============================================================================
// Constants and Macros
// ============================================================================

// Default initial capacity when the caller requests zero.
#define GROW_DEFAULT_CAP 4096

// ============================================================================
// Growable Buffer
// ============================================================================

// Ensure the growable buffer has room for at least `extra` more bytes.
static void grow_ensure(grow_buf_t *g, size_t extra, decode_ctx_t *ctx) {
    size_t needed = g->len + extra;
    if (needed <= g->cap) {
        return;
    }
    // Grow by at least 1.5x the current capacity
    size_t new_cap = g->cap + g->cap / 2;
    if (new_cap < needed) {
        new_cap = needed;
    }
    g->data = dctx_realloc(ctx, g->data, new_cap);
    g->cap = new_cap;
}

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

// Initialise a growable buffer with the given initial capacity.
void grow_init(grow_buf_t *g, size_t initial_cap, decode_ctx_t *ctx) {
    if (initial_cap == 0) {
        initial_cap = GROW_DEFAULT_CAP;
    }
    g->ctx = ctx;
    g->data = dctx_malloc(ctx, initial_cap);
    g->len = 0;
    g->cap = initial_cap;
}

// Append n bytes from src to the growable buffer.
void grow_append(grow_buf_t *g, const uint8_t *src, size_t n, decode_ctx_t *ctx) {
    if (n == 0) {
        return;
    }
    grow_ensure(g, n, ctx);
    memcpy(g->data + g->len, src, n);
    g->len += n;
}

// Append a single byte to the growable buffer.
void grow_push(grow_buf_t *g, uint8_t byte, decode_ctx_t *ctx) {
    grow_ensure(g, 1, ctx);
    g->data[g->len++] = byte;
}

// Turn the buffer into a peel_buf_t, then zero g.  The data stays registered
// with the decode context until the decoder releases it on success.
peel_buf_t grow_finish(grow_buf_t *g) {
    // Shrink to exact size to avoid wasting memory.  A plain realloc: a shrink
    // that fails keeps the oversized, still-valid buffer, and must not abort.
    if (g->len < g->cap && g->len > 0) {
        uint8_t *shrunk = realloc(g->data, g->len);
        if (shrunk) {
            dctx_rebind(g->ctx, g->data, shrunk); // the block may have moved
            g->data = shrunk;
        }
    }
    peel_buf_t result = {
        .data = g->data,
        .size = g->len,
        .owned = true,
    };
    // Zero the grow_buf so it cannot be reused
    memset(g, 0, sizeof(*g));
    return result;
}

// Release a growable buffer without producing a result (error cleanup).
void grow_free(grow_buf_t *g) {
    if (g->ctx)
        dctx_free(g->ctx, g->data);
    else
        free(g->data);
    memset(g, 0, sizeof(*g));
}
