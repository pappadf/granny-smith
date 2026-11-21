// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// byte_store.h
// The object-model face of a flat byte store: the peek / poke / dump /
// snapshot / restore methods that machine.rtc.pram (256 bytes) and
// machine.nvram (the 8 KB Open Firmware store of Grand Central and Heathrow)
// share.  Each store describes itself once in a byte_store_ops_t; the
// BYTE_STORE_METHODS macro stamps out the five method functions for it, and
// the store's own member table names them next to its other members (the
// docs and argument ranges are the store's own).
//
// peek returns its byte as hex (VFLAG_HEX) on every store.  A store with a
// write policy (the RTC's write-protect) gives a `write` hook: poke and
// restore then go through it byte by byte and stop at the first refusal.

#ifndef GS_CORE_OBJECT_BYTE_STORE_H
#define GS_CORE_OBJECT_BYTE_STORE_H

#include "object.h"
#include "value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One byte store, as the methods see it.
typedef struct byte_store_ops {
    uint8_t *(*store)(struct object *self); // the bytes; NULL -> "<missing>"
    bool (*write)(struct object *self, size_t off, uint8_t v); // NULL -> plain store; false = refused
    size_t size; // the store's length in bytes
    const char *what; // the error prefix, e.g. "nvram", "rtc.pram"
    const char *missing; // the error when store() is NULL, e.g. "rtc not available"
} byte_store_ops_t;

// The method bodies (argv as the member tables declare it).
value_t byte_store_peek(const byte_store_ops_t *ops, struct object *self, const value_t *argv); // (addr)
value_t byte_store_poke(const byte_store_ops_t *ops, struct object *self, const value_t *argv); // (addr, bytes)
value_t byte_store_dump(const byte_store_ops_t *ops, struct object *self, const value_t *argv); // (addr, n)
value_t byte_store_snapshot(const byte_store_ops_t *ops, struct object *self); // ()
value_t byte_store_restore(const byte_store_ops_t *ops, struct object *self, const value_t *argv); // (bytes)

// Define prefix##_peek, _poke, _dump, _snapshot and _restore as method_fn
// thunks over `ops` (a byte_store_ops_t object, not a pointer).
#define BYTE_STORE_METHODS(prefix, ops)                                                                                \
    static DEF_METHOD(prefix##_peek) {                                                                                 \
        return byte_store_peek(&(ops), self, argv);                                                                    \
    }                                                                                                                  \
    static DEF_METHOD(prefix##_poke) {                                                                                 \
        return byte_store_poke(&(ops), self, argv);                                                                    \
    }                                                                                                                  \
    static DEF_METHOD(prefix##_dump) {                                                                                 \
        return byte_store_dump(&(ops), self, argv);                                                                    \
    }                                                                                                                  \
    static DEF_METHOD(prefix##_snapshot) {                                                                             \
        return byte_store_snapshot(&(ops), self);                                                                      \
    }                                                                                                                  \
    static DEF_METHOD(prefix##_restore) {                                                                              \
        return byte_store_restore(&(ops), self, argv);                                                                 \
    }

#endif // GS_CORE_OBJECT_BYTE_STORE_H
