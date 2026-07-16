// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// internal.h
// Shared internal helpers for libpeeler format implementations.
// This header is NOT part of the public API.

#ifndef PEELER_INTERNAL_H
#define PEELER_INTERNAL_H

#include "peeler.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Error Creation — architecture.md § "Internal Error Creation"
// ============================================================================

// Allocate and populate a peel_err_t with a printf-style message.
peel_err_t *make_err(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// ============================================================================
// setjmp/longjmp Abort Context — architecture.md § "setjmp/longjmp"
// ============================================================================

// Jump-target context for deep-error abort in decompressors.
typedef struct {
    jmp_buf jmp;
    char errmsg[256];
} decode_ctx_t;

// Format a message into ctx->errmsg and longjmp back to the setjmp site.
void decode_abort(decode_ctx_t *ctx, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((noreturn, format(printf, 2, 3)))
#endif
    ;

// ============================================================================
// Big-Endian Read Helpers
// ============================================================================

// Read a big-endian 16-bit unsigned integer from a byte pointer.
static inline uint16_t rd16be(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

// Read a big-endian 32-bit unsigned integer from a byte pointer.
static inline uint32_t rd32be(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

// ============================================================================
// Big-Endian Write Helpers
// ============================================================================

// Write a 16-bit value in big-endian byte order.
static inline void wr16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

// Write a 32-bit value in big-endian byte order.
static inline void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

// ============================================================================
// CRC Routines
// ============================================================================

// CRC-16/CCITT (polynomial 0x1021, init 0) over a complete buffer.
uint16_t crc16_ccitt(const uint8_t *data, size_t len);

// Update a running CRC-16/CCITT with additional data.
uint16_t crc16_ccitt_update(uint16_t crc, const uint8_t *data, size_t len);

// ============================================================================
// Growable Buffer
// ============================================================================

// A dynamically growing output buffer for building results incrementally.
typedef struct {
    uint8_t *data; // Heap-allocated storage
    size_t len; // Number of valid bytes written
    size_t cap; // Allocated capacity in bytes
} grow_buf_t;

// Initialise a growable buffer with the given initial capacity.
// Aborts via ctx on allocation failure.
void grow_init(grow_buf_t *g, size_t initial_cap, decode_ctx_t *ctx);

// Append n bytes to the growable buffer, reallocating if needed.
void grow_append(grow_buf_t *g, const uint8_t *src, size_t n, decode_ctx_t *ctx);

// Append a single byte.
void grow_push(grow_buf_t *g, uint8_t byte, decode_ctx_t *ctx);

// Finalise the growable buffer into an owned peel_buf_t.  Zeroes g.
peel_buf_t grow_finish(grow_buf_t *g);

// Release a growable buffer without producing a peel_buf_t (for error paths).
void grow_free(grow_buf_t *g);

// ============================================================================
// Format Handler Registration — architecture.md § "Format Handler Registration"
// ============================================================================

// Classification of a format handler.
typedef enum {
    PEEL_FMT_WRAPPER, // One buffer in, one buffer out (e.g. HQX, MacBinary)
    PEEL_FMT_ARCHIVE, // One buffer in, file list out  (e.g. StuffIt, CPT)
} peel_fmt_kind_t;

// A registered format handler entry in the detection table.
typedef struct {
    const char *name;
    peel_fmt_kind_t kind;
    bool (*detect)(const uint8_t *src, size_t len);
    peel_buf_t (*peel_wrapper)(const uint8_t *src, size_t len, peel_err_t **err);
    peel_file_list_t (*peel_archive)(const uint8_t *src, size_t len, peel_err_t **err);
} peel_format_t;

// ============================================================================
// Per-Format Detect Functions
// ============================================================================

// Each format source file defines its own detect function for the handler table.

bool hqx_detect(const uint8_t *src, size_t len);

bool bin_detect(const uint8_t *src, size_t len);

bool sit_detect(const uint8_t *src, size_t len);

bool cpt_detect(const uint8_t *src, size_t len);

#endif // PEELER_INTERNAL_H
