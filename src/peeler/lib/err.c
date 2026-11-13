// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// err.c
// Error object creation and formatting.

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
