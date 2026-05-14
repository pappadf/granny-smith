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
    value_t v = {0};
    v.kind = V_LIST;
    v.list.items = items;
    v.list.len = len;
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

bool val_is_heap(const value_t *v) {
    if (!v)
        return false;
    switch (v->kind) {
    case V_STRING:
    case V_BYTES:
    case V_LIST:
    case V_ERROR:
        return true;
    default:
        return false;
    }
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
    case V_OBJECT:
        return v->obj != NULL;
    case V_NONE:
        return false;
    case V_ERROR:
        return false;
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
    default:
        break;
    }
    return r;
}
