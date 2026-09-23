// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// err.c
// Error object creation, formatting, and setjmp/longjmp abort helper.

#include "internal.h"

// ============================================================================
// Type Definitions (Private)
// ============================================================================

// Concrete definition of the opaque error struct declared in peeler.h.
struct peel_err {
    char message[512];
};

// ============================================================================
// Static Helpers
// ============================================================================

// Allocate and populate an error object with a printf-style message.
peel_err_t *make_err(const char *fmt, ...) {
    peel_err_t *e = malloc(sizeof(*e));
    if (!e) {
        // OOM while reporting an error — nothing useful we can do
        return NULL;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, ap);
    va_end(ap);
    return e;
}

// Format a message into the decode context and longjmp to the error handler.
void decode_abort(decode_ctx_t *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->errmsg, sizeof(ctx->errmsg), fmt, ap);
    va_end(ap);
    longjmp(ctx->jmp, 1);
}

// ============================================================================
// Decode-context allocation ownership (see decode_ctx_t in internal.h)
// ============================================================================

void dctx_init(decode_ctx_t *ctx) {
    ctx->n_owned = 0;
    ctx->errmsg[0] = '\0';
}

static void dctx_register(decode_ctx_t *ctx, void *p) {
    if (ctx->n_owned >= DCTX_MAX_OWNED) {
        free(p);
        decode_abort(ctx, "internal: more than %d live decoder allocations", DCTX_MAX_OWNED);
    }
    ctx->owned[ctx->n_owned++] = p;
}

static int dctx_find(const decode_ctx_t *ctx, const void *p) {
    for (int i = 0; i < ctx->n_owned; i++)
        if (ctx->owned[i] == p)
            return i;
    return -1;
}

static void dctx_forget(decode_ctx_t *ctx, int i) {
    ctx->owned[i] = ctx->owned[--ctx->n_owned];
}

void *dctx_malloc(decode_ctx_t *ctx, size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p)
        decode_abort(ctx, "out of memory allocating %zu bytes", size);
    dctx_register(ctx, p);
    return p;
}

void *dctx_calloc(decode_ctx_t *ctx, size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p)
        decode_abort(ctx, "out of memory allocating %zu x %zu bytes", n, size);
    dctx_register(ctx, p);
    return p;
}

void *dctx_realloc(decode_ctx_t *ctx, void *p, size_t size) {
    int i = p ? dctx_find(ctx, p) : -1;
    void *q = realloc(p, size ? size : 1);
    if (!q)
        decode_abort(ctx, "out of memory reallocating to %zu bytes", size); // p still registered
    if (i >= 0)
        ctx->owned[i] = q;
    else
        dctx_register(ctx, q);
    return q;
}

void dctx_rebind(decode_ctx_t *ctx, void *old, void *now) {
    int i = dctx_find(ctx, old);
    if (i >= 0)
        ctx->owned[i] = now;
    else
        dctx_register(ctx, now);
}

void dctx_free(decode_ctx_t *ctx, void *p) {
    if (!p)
        return;
    int i = dctx_find(ctx, p);
    if (i >= 0)
        dctx_forget(ctx, i);
    free(p);
}

void *dctx_release(decode_ctx_t *ctx, void *p) {
    if (p) {
        int i = dctx_find(ctx, p);
        if (i >= 0)
            dctx_forget(ctx, i);
    }
    return p;
}

void dctx_cleanup(decode_ctx_t *ctx) {
    for (int i = 0; i < ctx->n_owned; i++)
        free(ctx->owned[i]);
    ctx->n_owned = 0;
}

// ============================================================================
// Operations (Public API)
// ============================================================================

// Return the error message string, or a generic fallback for NULL.
const char *peel_err_msg(const peel_err_t *err) {
    if (!err) {
        return "(no error)";
    }
    return err->message;
}

// Free an error object.  Safe to call with NULL.
void peel_err_free(peel_err_t *err) {
    free(err);
}
