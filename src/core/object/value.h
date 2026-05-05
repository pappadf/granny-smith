// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// value.h
// Tagged-union value type used across every object-model boundary.
// See local/gs-docs/notes/proposal-module-object-model.md §2.4–§2.6.
//
// Ownership rule (single-owner): the receiver of a value_t owns it.
// value_free is safe to call on every kind, including inline kinds.
// String/error/bytes/list kinds heap-allocate; constructors strdup
// their inputs, so there is no borrowed-string path.

#ifndef GS_OBJECT_VALUE_H
#define GS_OBJECT_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

// Discriminator for the value_t union.
typedef enum {
    V_NONE = 0, // success, nothing to say (also default-init)
    V_BOOL,
    V_INT, // signed, up to 64-bit
    V_UINT, // unsigned, up to 64-bit (default for addresses, registers)
    V_FLOAT, // double
    V_STRING, // heap-owned char*
    V_BYTES, // heap-owned opaque byte buffer
    V_ENUM, // integer + static name table
    V_LIST, // heap-owned, recursively owned items
    V_OBJECT, // non-owning reference to a tree node
    V_ERROR, // heap-owned error message
} value_kind_t;

// Display / semantic flags. Stored on attribute member_t and copied onto
// value_t so formatters see the intent of the originating attribute.
#define VAL_HEX       0x0001u // prefer hex output
#define VAL_DEC       0x0002u // prefer decimal output
#define VAL_VOLATILE  0x0004u // re-read every time (no caching)
#define VAL_SENSITIVE 0x0008u // do not print payload (passwords, etc.)
#define VAL_RO        0x0010u // attribute is read-only
#define VAL_BIN       0x0020u // prefer binary output

// Forward declaration; defined in object.h.
struct object;

// Tagged union value; passed by value across boundaries.
typedef struct value {
    value_kind_t kind;
    uint8_t width; // bytes: 1, 2, 4, 8, 10 (FPU ext); 0 otherwise
    uint16_t flags; // VAL_HEX | VAL_DEC | VAL_VOLATILE | ...
    union {
        bool b;
        int64_t i;
        uint64_t u;
        double f;
        char *s; // heap-owned; freed by value_free
        struct {
            uint8_t *p;
            size_t n;
        } bytes; // p heap-owned
        struct {
            int idx;
            const char *const *table;
            size_t n_table;
        } enm;
        struct {
            struct value *items;
            size_t len;
        } list;
        struct object *obj; // non-owning
        char *err; // heap-owned
    };
} value_t;

// === Constructors ============================================================

// All string-taking constructors strdup their input.

value_t val_none(void);
value_t val_bool(bool b);
value_t val_int(int64_t i);
value_t val_uint(uint8_t width, uint64_t u);
value_t val_float(double f);
value_t val_str(const char *s);
value_t val_bytes(const void *p, size_t n);
value_t val_enum(int idx, const char *const *table, size_t n_table);
value_t val_list(value_t *items, size_t len); // takes ownership of items array
value_t val_obj(struct object *o);
value_t val_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Convenience constants.
#define V_NONE_VAL (val_none())

// === Lifetime ================================================================

// Free any heap storage owned by *v and zero the discriminant. Safe for inline
// kinds (no-op). Safe to call repeatedly. Safe to call on a fresh
// (zero-initialised) value_t.
void value_free(value_t *v);

// GCC/Clang cleanup attribute helper, used as:
//   VALUE_AUTO value_t x = something();
// to release on scope exit including error paths.
#define VALUE_AUTO __attribute__((cleanup(value_free_ptr))) value_t
void value_free_ptr(value_t *v); // wrapper compatible with cleanup attribute

// === Accessors ===============================================================

// Coerce to u64. For numeric kinds this is the natural conversion; for
// V_BOOL it is 0/1; for V_ENUM it is the index. Returns 0 with *ok=false
// on incompatible kinds (V_STRING, V_BYTES, V_LIST, V_OBJECT, V_ERROR,
// V_NONE). Caller may pass ok=NULL to ignore the success flag.
uint64_t val_as_u64(const value_t *v, bool *ok);
int64_t val_as_i64(const value_t *v, bool *ok);
double val_as_f64(const value_t *v, bool *ok);
bool val_as_bool(const value_t *v); // truthiness (proposal §2.5)

// Borrowed pointer to the string body or NULL. The pointer is only valid
// while *v is alive.
const char *val_as_str(const value_t *v);

// True if *v carries an error.
static inline bool val_is_error(const value_t *v) {
    return v && v->kind == V_ERROR;
}

// True if *v is one of the heap-owning kinds.
bool val_is_heap(const value_t *v);

// Deep copy. Inline kinds are returned by value; heap-owning kinds duplicate
// their storage. Lists recurse.
value_t value_copy(const value_t *v);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_VALUE_H
