// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// value.c
// Tagged-union value type. See value.h for the contract.

#include "value.h"
#include "gs_assert.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

value_t val_none(void) {
    value_t v = {0};
    v.kind = VK_NONE;
    return v;
}

value_t val_bool(bool b) {
    value_t v = {0};
    v.kind = VK_BOOL;
    v.width = 1;
    v.b = b;
    return v;
}

value_t val_int(int64_t i) {
    value_t v = {0};
    v.kind = VK_INT;
    v.width = 8;
    v.i = i;
    return v;
}

value_t val_uint(uint8_t width, uint64_t u) {
    value_t v = {0};
    v.kind = VK_UINT;
    v.width = width; // 0 = unconstrained, kept as given
    v.u = u;
    return v;
}

value_t val_float(double f) {
    value_t v = {0};
    v.kind = VK_FLOAT;
    v.width = 8;
    v.f = f;
    return v;
}

value_t val_str(const char *s) {
    value_t v = {0};
    v.kind = VK_STRING;
    v.s = strdup(s ? s : "");
    return v;
}

value_t val_bytes(const void *p, size_t n) {
    value_t v = {0};
    v.kind = VK_BYTES;
    v.bytes.n = 0;
    v.bytes.p = NULL;
    if (n == 0)
        return v;
    // A NULL source with a non-zero length is a caller bug, not a request
    // for zeroes; report it like every other invalid constructor input.
    if (!p)
        return val_err("val_bytes: NULL source for %zu bytes", n);
    v.bytes.p = (uint8_t *)malloc(n);
    if (!v.bytes.p)
        return v; // length stays 0: p != NULL whenever n > 0, always
    memcpy(v.bytes.p, p, n);
    v.bytes.n = n;
    return v;
}

value_t val_enum(int idx, const char *const *table, size_t n_table) {
    value_t v = {0};
    v.kind = VK_ENUM;
    v.enm.idx = idx;
    v.enm.table = table;
    v.enm.n_table = n_table;
    return v;
}

value_t val_list(value_t *items, size_t len) {
    GS_ASSERT(len == 0 || items != NULL);
    value_t v = {0};
    v.kind = VK_LIST;
    v.list.items = items;
    v.list.len = len;
    return v;
}

value_t val_map(struct value_entry *entries, size_t len) {
    GS_ASSERT(len == 0 || entries != NULL);
    value_t v = {0};
    v.kind = VK_MAP;
    v.map.entries = entries;
    v.map.len = len;
    return v;
}

value_t val_obj(struct object *o) {
    value_t v = {0};
    v.kind = VK_OBJECT;
    v.obj = o;
    return v;
}

value_t val_err(const char *fmt, ...) {
    value_t v = {0};
    v.kind = VK_ERROR;
    char buf[512];
    va_list ap;
    buf[0] = '\0';
    va_start(ap, fmt);
    int n = fmt ? vsnprintf(buf, sizeof(buf), fmt, ap) : 0;
    va_end(ap);
    // Over-long message: keep the fixed cap but mark the cut with "...".
    if (n >= (int)sizeof(buf))
        memcpy(buf + sizeof(buf) - 4, "...", 4);
    v.err = strdup(buf);
    return v;
}

value_t val_ref(const char *path) {
    value_t v = {0};
    v.kind = VK_REF;
    v.ref = strdup(path ? path : "");
    return v;
}

value_t val_range_step(int64_t start, int64_t stop, int64_t step) {
    value_t v = {0};
    v.kind = VK_RANGE;
    v.range.start = start;
    v.range.stop = stop;
    v.range.step = step ? step : 1;
    return v;
}

value_t val_range(int64_t start, int64_t stop) {
    return val_range_step(start, stop, 1);
}

uint64_t val_range_count(const value_t *v) {
    if (!v || v->kind != VK_RANGE)
        return 0;
    int64_t step = v->range.step ? v->range.step : 1;
    // Compute the span in UINT64 so INT64_MIN..INT64_MAX cannot overflow the
    // subtraction, which is undefined in int64.
    if (step > 0) {
        if (v->range.stop <= v->range.start)
            return 0;
        uint64_t span = (uint64_t)v->range.stop - (uint64_t)v->range.start;
        return (span + (uint64_t)step - 1u) / (uint64_t)step;
    }
    if (v->range.stop >= v->range.start)
        return 0;
    uint64_t span = (uint64_t)v->range.start - (uint64_t)v->range.stop;
    uint64_t mag = (uint64_t)(-(step + 1)) + 1u; // |step|, safe for INT64_MIN
    return (span + mag - 1u) / mag;
}

bool val_is_heap(const value_t *v) {
    if (!v)
        return false;
    switch (v->kind) {
    case VK_STRING:
    case VK_BYTES:
    case VK_LIST:
    case VK_MAP:
    case VK_ERROR:
    case VK_REF:
        return true;
    default:
        return false;
    }
}

// === Map builder =============================================================

// Growable entry array plus a sticky error flag (val_map_finish surfaces
// it as VK_ERROR so per-put checks aren't needed at call sites).
struct value_map_builder {
    struct value_entry *entries;
    size_t len;
    size_t cap;
    bool err;
};

value_map_builder_t *val_map_new(void) {
    return (value_map_builder_t *)calloc(1, sizeof(value_map_builder_t));
}

void val_map_put(value_map_builder_t *b, const char *key, value_t v) {
    if (!b || b->err) {
        value_free(&v);
        return;
    }
    // Duplicate key: replace the existing value in place (keys unique).
    for (size_t i = 0; i < b->len; i++) {
        if (strcmp(b->entries[i].key, key ? key : "") == 0) {
            value_free(&b->entries[i].val);
            b->entries[i].val = v;
            return;
        }
    }
    if (b->len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8;
        struct value_entry *e = (struct value_entry *)realloc(b->entries, cap * sizeof(*e));
        if (!e) {
            b->err = true;
            value_free(&v);
            return;
        }
        b->entries = e;
        b->cap = cap;
    }
    char *k = strdup(key ? key : "");
    if (!k) {
        b->err = true;
        value_free(&v);
        return;
    }
    b->entries[b->len].key = k;
    b->entries[b->len].val = v;
    b->len++;
}

value_t val_map_finish(value_map_builder_t *b) {
    if (!b)
        return val_err("map builder: out of memory");
    if (b->err) {
        // Free the partial map and report the sticky allocation failure.
        for (size_t i = 0; i < b->len; i++) {
            free(b->entries[i].key);
            value_free(&b->entries[i].val);
        }
        free(b->entries);
        free(b);
        return val_err("map builder: out of memory");
    }
    value_t v = val_map(b->entries, b->len);
    free(b);
    return v;
}

bool val_list_push(value_t **items, size_t *len, size_t *cap, value_t v) {
    if (*len + 1 > *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        value_t *ni = (value_t *)realloc(*items, nc * sizeof(value_t));
        if (!ni) {
            value_free(&v);
            return false;
        }
        *items = ni;
        *cap = nc;
    }
    (*items)[(*len)++] = v;
    return true;
}

const value_t *value_map_get(const value_t *v, const char *key) {
    if (!v || v->kind != VK_MAP || !key)
        return NULL;
    for (size_t i = 0; i < v->map.len; i++) {
        if (v->map.entries[i].key && strcmp(v->map.entries[i].key, key) == 0)
            return &v->map.entries[i].val;
    }
    return NULL;
}

void value_free(value_t *v) {
    if (!v)
        return;
    switch (v->kind) {
    case VK_STRING:
        free(v->s);
        v->s = NULL;
        break;
    case VK_ERROR:
        free(v->err);
        v->err = NULL;
        break;
    case VK_REF:
        free(v->ref);
        v->ref = NULL;
        break;
    case VK_BYTES:
        free(v->bytes.p);
        v->bytes.p = NULL;
        v->bytes.n = 0;
        break;
    case VK_LIST:
        if (v->list.items) {
            for (size_t i = 0; i < v->list.len; i++)
                value_free(&v->list.items[i]);
            free(v->list.items);
        }
        v->list.items = NULL;
        v->list.len = 0;
        break;
    case VK_MAP:
        if (v->map.entries) {
            for (size_t i = 0; i < v->map.len; i++) {
                free(v->map.entries[i].key);
                value_free(&v->map.entries[i].val);
            }
            free(v->map.entries);
        }
        v->map.entries = NULL;
        v->map.len = 0;
        break;
    default:
        break;
    }
    v->kind = VK_NONE;
    v->width = 0;
    v->flags = 0;
}

void value_free_ptr(value_t *v) {
    value_free(v);
}

value_t value_dup(const value_t *v) {
    if (!v)
        return val_none();
    switch (v->kind) {
    case VK_STRING:
        return val_str(v->s ? v->s : "");
    case VK_ERROR:
        return val_err("%s", v->err ? v->err : "");
    case VK_REF:
        return val_ref(v->ref ? v->ref : "");
    case VK_BYTES:
        return val_bytes(v->bytes.p, v->bytes.n);
    case VK_LIST: {
        value_t *items = NULL;
        if (v->list.len > 0) {
            items = (value_t *)calloc(v->list.len, sizeof(value_t));
            if (!items)
                return val_err("value_dup: OOM duplicating list of %zu", v->list.len);
            for (size_t i = 0; i < v->list.len; i++)
                items[i] = value_dup(&v->list.items[i]);
        }
        return val_list(items, v->list.len);
    }
    case VK_MAP: {
        struct value_entry *entries = NULL;
        if (v->map.len > 0) {
            entries = (struct value_entry *)calloc(v->map.len, sizeof(*entries));
            if (!entries)
                return val_err("value_dup: OOM duplicating map of %zu", v->map.len);
            for (size_t i = 0; i < v->map.len; i++) {
                entries[i].key = strdup(v->map.entries[i].key);
                entries[i].val = value_dup(&v->map.entries[i].val);
            }
        }
        value_t r = val_map(entries, v->map.len);
        r.flags = v->flags;
        return r;
    }
    default:
        // Inline kinds (VK_NONE, VK_BOOL, VK_INT, VK_UINT, VK_FLOAT, VK_ENUM,
        // VK_OBJECT) carry their payload by value — a structure copy is
        // a complete duplicate.
        return *v;
    }
}

uint64_t val_as_u64(const value_t *v, bool *ok) {
    if (!v) {
        if (ok)
            *ok = false;
        return 0;
    }
    if (ok)
        *ok = true;
    switch (v->kind) {
    case VK_BOOL:
        return v->b ? 1u : 0u;
    case VK_INT:
        return (uint64_t)v->i;
    case VK_UINT:
        return v->u;
    case VK_FLOAT:
        return (uint64_t)v->f;
    case VK_ENUM:
        return (uint64_t)v->enm.idx;
    default:
        if (ok)
            *ok = false;
        return 0;
    }
}

int64_t val_as_i64(const value_t *v, bool *ok) {
    if (!v) {
        if (ok)
            *ok = false;
        return 0;
    }
    if (ok)
        *ok = true;
    switch (v->kind) {
    case VK_BOOL:
        return v->b ? 1 : 0;
    case VK_INT:
        return v->i;
    case VK_UINT:
        return (int64_t)v->u;
    case VK_FLOAT:
        return (int64_t)v->f;
    case VK_ENUM:
        return (int64_t)v->enm.idx;
    default:
        if (ok)
            *ok = false;
        return 0;
    }
}

double val_as_f64(const value_t *v, bool *ok) {
    if (!v) {
        if (ok)
            *ok = false;
        return 0.0;
    }
    if (ok)
        *ok = true;
    switch (v->kind) {
    case VK_BOOL:
        return v->b ? 1.0 : 0.0;
    case VK_INT:
        return (double)v->i;
    case VK_UINT:
        return (double)v->u;
    case VK_FLOAT:
        return v->f;
    case VK_ENUM:
        return (double)v->enm.idx;
    default:
        if (ok)
            *ok = false;
        return 0.0;
    }
}

// Truthiness, per kind.
bool val_as_bool(const value_t *v) {
    if (!v)
        return false;
    switch (v->kind) {
    case VK_BOOL:
        return v->b;
    case VK_INT:
        return v->i != 0;
    case VK_UINT:
        return v->u != 0;
    case VK_FLOAT:
        return v->f != 0.0 && !isnan(v->f);
    case VK_STRING:
        return v->s && v->s[0] != '\0';
    case VK_BYTES:
        return v->bytes.n > 0;
    case VK_ENUM:
        return v->enm.idx != 0;
    case VK_LIST:
        return v->list.len > 0;
    case VK_MAP:
        return v->map.len > 0;
    case VK_OBJECT:
        return v->obj != NULL;
    case VK_NONE:
        return false;
    case VK_ERROR:
        return false;
    case VK_REF:
        return v->ref != NULL; // a reference is truthy; deref happens before tests
    case VK_RANGE:
        return v->range.stop > v->range.start; // non-empty range
    }
    return false;
}

const char *val_as_str(const value_t *v) {
    if (!v)
        return NULL;
    if (v->kind == VK_STRING)
        return v->s;
    if (v->kind == VK_ERROR)
        return v->err;
    return NULL;
}

const char *value_kind_name(value_kind_t k) {
    if (k == VK_ANY) // a declaration-only sentinel outside the enum
        return "any";
    switch (k) {
    case VK_NONE:
        return "none";
    case VK_BOOL:
        return "bool";
    case VK_INT:
        return "int";
    case VK_UINT:
        return "uint";
    case VK_FLOAT:
        return "float";
    case VK_STRING:
        return "string";
    case VK_BYTES:
        return "bytes";
    case VK_ENUM:
        return "enum";
    case VK_LIST:
        return "list";
    case VK_MAP:
        return "map";
    case VK_OBJECT:
        return "object";
    case VK_ERROR:
        return "error";
    case VK_REF:
        return "ref";
    case VK_RANGE:
        return "range";
    }
    return "?";
}

bool val_parse_bool(const char *s, bool *out) {
    if (!s || !out)
        return false;
    static const char *const yes[] = {"true", "on", "yes", "1", NULL};
    static const char *const no[] = {"false", "off", "no", "0", NULL};
    for (int i = 0; yes[i]; i++)
        if (strcmp(s, yes[i]) == 0) {
            *out = true;
            return true;
        }
    for (int i = 0; no[i]; i++)
        if (strcmp(s, no[i]) == 0) {
            *out = false;
            return true;
        }
    return false;
}
