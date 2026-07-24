// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// value.h
// Tagged-union value type used across every object-model boundary.
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
    V_MAP, // heap-owned, ordered {key → recursively-owned value} entries
    V_OBJECT, // non-owning reference to a tree node
    V_ERROR, // heap-owned error message
    V_REF, // heap-owned object-tree path text; re-resolved on every access
    V_RANGE, // half-open integer range [start, stop)
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

// One V_MAP entry: heap-owned key plus recursively-owned value. Defined
// after struct value (it embeds one by value); forward-declared here so
// the union arm can carry the pointer.
struct value_entry;

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
        struct {
            struct value_entry *entries;
            size_t len;
        } map; // entries heap-owned; insertion-ordered, keys unique
        struct object *obj; // non-owning
        char *err; // heap-owned
        char *ref; // heap-owned path text (V_REF); freed by value_free
        struct {
            int64_t start;
            int64_t stop;
        } range; // half-open [start, stop)
    };
} value_t;

// One {key → value} slot of a V_MAP. The key is heap-owned (strdup'd by
// the constructors); the value is recursively owned like a list item.
struct value_entry {
    char *key;
    struct value val;
};

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
value_t val_map(struct value_entry *entries, size_t len); // takes ownership of entries array
value_t val_obj(struct object *o);
value_t val_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
value_t val_ref(const char *path); // node reference by path text (strdup'd)
value_t val_range(int64_t start, int64_t stop); // half-open [start, stop)

// Convenience constants.
#define V_NONE_VAL (val_none())

// === Map builder =============================================================
//
// Incremental V_MAP construction mirroring the shape the retired JSON
// builder had, so map-returning method bodies stay a flat put-sequence.
// The builder owns everything handed to it; val_map_finish transfers the
// finished map (or a V_ERROR after any OOM) and frees the builder either
// way. Errors are sticky: a failed put poisons the builder and finish
// reports it.

typedef struct value_map_builder value_map_builder_t;

// Allocate a fresh builder. Returns NULL on OOM (val_map_finish(NULL)
// yields a V_ERROR, so call sites may chain without checking).
value_map_builder_t *val_map_new(void);

// Append {key → v}. The key is strdup'd; v is owned by the builder from
// this call on (also on failure). A duplicate key replaces the earlier
// value in place, keeping keys unique and order stable.
void val_map_put(value_map_builder_t *b, const char *key, value_t v);

// Consume the builder and return the finished V_MAP (possibly empty),
// or V_ERROR if any allocation failed along the way.
value_t val_map_finish(value_map_builder_t *b);

// Borrowed lookup: pointer to the value stored under `key`, or NULL if
// `v` is not a V_MAP or the key is absent. Valid only while *v lives.
const value_t *value_map_get(const value_t *v, const char *key);

// Append v to a growable value_t array (doubling capacity), the common
// idiom for assembling a V_LIST of unknown length before val_list. On
// OOM frees v and returns false; the array so far stays valid so the
// caller can free the partial list.
bool val_list_push(value_t **items, size_t *len, size_t *cap, value_t v);

// === Lifetime ================================================================

// Free any heap storage owned by *v and zero the discriminant. Safe for inline
// kinds (no-op). Safe to call repeatedly. Safe to call on a fresh
// (zero-initialised) value_t.
void value_free(value_t *v);

// Deep-copy a value.  The returned value owns its own heap storage so the
// caller can free it independently of `v`.  Used by the shell-variable
// binding to hand out owned copies to expression callers without sharing
// pointer-typed payloads (V_STRING, V_BYTES, V_LIST, V_ERROR).
value_t value_dup(const value_t *v);

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
