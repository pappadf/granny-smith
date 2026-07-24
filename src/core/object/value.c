// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// value.c
// Tagged-union value type. See value.h for the contract.

#include "value.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

// Duplicate a NUL-terminated string with malloc. NULL-safe.
static char *xstrdup(const char *s) {
    if (!s)
        return NULL;
    size_t n = strlen(s);
    char *r = (char *)malloc(n + 1);
    if (!r)
        return NULL;
    memcpy(r, s, n + 1);
    return r;
}

value_t val_none(void) {
    value_t v = {0};
    v.kind = V_NONE;
    return v;
}

value_t val_bool(bool b) {
    value_t v = {0};
    v.kind = V_BOOL;
    v.width = 1;
    v.b = b;
    return v;
}

value_t val_int(int64_t i) {
    value_t v = {0};
    v.kind = V_INT;
    v.width = 8;
    v.i = i;
    return v;
}

value_t val_uint(uint8_t width, uint64_t u) {
    value_t v = {0};
    v.kind = V_UINT;
    v.width = width ? width : 8;
    v.u = u;
    return v;
}

value_t val_float(double f) {
    value_t v = {0};
    v.kind = V_FLOAT;
    v.width = 8;
    v.f = f;
    return v;
}

value_t val_str(const char *s) {
    value_t v = {0};
    v.kind = V_STRING;
    v.s = xstrdup(s ? s : "");
    return v;
}

value_t val_bytes(const void *p, size_t n) {
    value_t v = {0};
    v.kind = V_BYTES;
    v.bytes.n = n;
    if (n > 0) {
        v.bytes.p = (uint8_t *)malloc(n);
        if (v.bytes.p && p)
            memcpy(v.bytes.p, p, n);
        else if (v.bytes.p)
            memset(v.bytes.p, 0, n);
    } else {
        v.bytes.p = NULL;
    }
    return v;
}

value_t val_enum(int idx, const char *const *table, size_t n_table) {
    value_t v = {0};
    v.kind = V_ENUM;
    v.enm.idx = idx;
    v.enm.table = table;
    v.enm.n_table = n_table;
    return v;
}

value_t val_list(value_t *items, size_t len) {
    GS_ASSERT(len == 0 || items != NULL);
    value_t v = {0};
    v.kind = V_LIST;
    v.list.items = items;
    v.list.len = len;
    return v;
}

value_t val_map(struct value_entry *entries, size_t len) {
    GS_ASSERT(len == 0 || entries != NULL);
    value_t v = {0};
    v.kind = V_MAP;
    v.map.entries = entries;
    v.map.len = len;
    return v;
}

value_t val_obj(struct object *o) {
    value_t v = {0};
    v.kind = V_OBJECT;
    v.obj = o;
    return v;
}

value_t val_err(const char *fmt, ...) {
    value_t v = {0};
    v.kind = V_ERROR;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
    va_end(ap);
    v.err = xstrdup(buf);
    return v;
}

value_t val_ref(const char *path) {
    value_t v = {0};
    v.kind = V_REF;
    v.ref = xstrdup(path ? path : "");
    return v;
}

value_t val_range(int64_t start, int64_t stop) {
    value_t v = {0};
    v.kind = V_RANGE;
    v.range.start = start;
    v.range.stop = stop;
    return v;
}

bool val_is_heap(const value_t *v) {
    if (!v)
        return false;
    switch (v->kind) {
    case V_STRING:
    case V_BYTES:
    case V_LIST:
    case V_MAP:
    case V_ERROR:
    case V_REF:
        return true;
    default:
        return false;
    }
}

// === Map builder =============================================================

// Growable entry array plus a sticky error flag (val_map_finish surfaces
// it as V_ERROR so per-put checks aren't needed at call sites).
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
    char *k = xstrdup(key ? key : "");
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
    if (!v || v->kind != V_MAP || !key)
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
    case V_STRING:
        free(v->s);
        v->s = NULL;
        break;
    case V_ERROR:
        free(v->err);
        v->err = NULL;
        break;
    case V_REF:
        free(v->ref);
        v->ref = NULL;
        break;
    case V_BYTES:
        free(v->bytes.p);
        v->bytes.p = NULL;
        v->bytes.n = 0;
        break;
    case V_LIST:
        if (v->list.items) {
            for (size_t i = 0; i < v->list.len; i++)
                value_free(&v->list.items[i]);
            free(v->list.items);
        }
        v->list.items = NULL;
        v->list.len = 0;
        break;
    case V_MAP:
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
    v->kind = V_NONE;
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
    case V_STRING:
        return val_str(v->s ? v->s : "");
    case V_ERROR:
        return val_err("%s", v->err ? v->err : "");
    case V_REF:
        return val_ref(v->ref ? v->ref : "");
    case V_BYTES:
        return val_bytes(v->bytes.p, v->bytes.n);
    case V_LIST: {
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
    case V_MAP: {
        struct value_entry *entries = NULL;
        if (v->map.len > 0) {
            entries = (struct value_entry *)calloc(v->map.len, sizeof(*entries));
            if (!entries)
                return val_err("value_dup: OOM duplicating map of %zu", v->map.len);
            for (size_t i = 0; i < v->map.len; i++) {
                entries[i].key = xstrdup(v->map.entries[i].key);
                entries[i].val = value_dup(&v->map.entries[i].val);
            }
        }
        value_t r = val_map(entries, v->map.len);
        r.flags = v->flags;
        return r;
    }
    default:
        // Inline kinds (V_NONE, V_BOOL, V_INT, V_UINT, V_FLOAT, V_ENUM,
        // V_OBJECT) carry their payload by value — a structure copy is
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
    case V_BOOL:
        return v->b ? 1u : 0u;
    case V_INT:
        return (uint64_t)v->i;
    case V_UINT:
        return v->u;
    case V_FLOAT:
        return (uint64_t)v->f;
    case V_ENUM:
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
    case V_BOOL:
        return v->b ? 1 : 0;
    case V_INT:
        return v->i;
    case V_UINT:
        return (int64_t)v->u;
    case V_FLOAT:
        return (int64_t)v->f;
    case V_ENUM:
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
    case V_BOOL:
        return v->b ? 1.0 : 0.0;
    case V_INT:
        return (double)v->i;
    case V_UINT:
        return (double)v->u;
    case V_FLOAT:
        return v->f;
    case V_ENUM:
        return (double)v->enm.idx;
    default:
        if (ok)
            *ok = false;
        return 0.0;
    }
}

// Truthiness rule per proposal-shell-expressions.md §2.5.
bool val_as_bool(const value_t *v) {
    if (!v)
        return false;
    switch (v->kind) {
    case V_BOOL:
        return v->b;
    case V_INT:
        return v->i != 0;
    case V_UINT:
        return v->u != 0;
    case V_FLOAT:
        return v->f != 0.0 && !isnan(v->f);
    case V_STRING:
        return v->s && v->s[0] != '\0';
    case V_BYTES:
        return v->bytes.n > 0;
    case V_ENUM:
        return v->enm.idx != 0;
    case V_LIST:
        return v->list.len > 0;
    case V_MAP:
        return v->map.len > 0;
    case V_OBJECT:
        return v->obj != NULL;
    case V_NONE:
        return false;
    case V_ERROR:
        return false;
    case V_REF:
        return v->ref != NULL; // a reference is truthy; deref happens before tests
    case V_RANGE:
        return v->range.stop > v->range.start; // non-empty range
    }
    return false;
}

const char *val_as_str(const value_t *v) {
    if (!v)
        return NULL;
    if (v->kind == V_STRING)
        return v->s;
    if (v->kind == V_ERROR)
        return v->err;
    return NULL;
}

value_t value_copy(const value_t *v) {
    if (!v)
        return val_none();
    value_t r = *v;
    switch (v->kind) {
    case V_STRING:
        r.s = xstrdup(v->s);
        break;
    case V_ERROR:
        r.err = xstrdup(v->err);
        break;
    case V_REF:
        r.ref = xstrdup(v->ref);
        break;
    case V_BYTES:
        if (v->bytes.n > 0) {
            r.bytes.p = (uint8_t *)malloc(v->bytes.n);
            if (r.bytes.p)
                memcpy(r.bytes.p, v->bytes.p, v->bytes.n);
        }
        break;
    case V_LIST:
        if (v->list.len > 0) {
            r.list.items = (value_t *)malloc(v->list.len * sizeof(value_t));
            if (r.list.items) {
                for (size_t i = 0; i < v->list.len; i++)
                    r.list.items[i] = value_copy(&v->list.items[i]);
            } else {
                r.list.len = 0;
            }
        }
        break;
    case V_MAP:
        if (v->map.len > 0) {
            r.map.entries = (struct value_entry *)calloc(v->map.len, sizeof(*r.map.entries));
            if (r.map.entries) {
                for (size_t i = 0; i < v->map.len; i++) {
                    r.map.entries[i].key = xstrdup(v->map.entries[i].key);
                    r.map.entries[i].val = value_copy(&v->map.entries[i].val);
                }
            } else {
                r.map.len = 0;
            }
        }
        break;
    default:
        break;
    }
    return r;
}
