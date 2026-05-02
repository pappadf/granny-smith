// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// alias.c
// Two-tier alias table. See alias.h for the contract.
//
// Storage: a small dynamic array of (name, path, kind) entries with
// linear lookup. We expect ~500 aliases (471 mac globals + ~30
// CPU/FPU registers); a hash table is overkill at this size and the
// table is mostly populated once at boot, then queried on every
// `$name` resolution. A linear scan over ~500 entries is sub-µs.

#include "alias.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object.h" // object_is_reserved_word, object_validate_name

typedef struct {
    char *name;
    char *path;
    alias_kind_t kind;
} alias_entry_t;

static alias_entry_t *g_table = NULL;
static size_t g_count = 0;
static size_t g_capacity = 0;

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

static void set_err(char *err_buf, size_t err_size, const char *fmt, ...) {
    if (!err_buf || !err_size)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_buf, err_size, fmt, ap);
    va_end(ap);
}

// Linear lookup. Returns the index in g_table or -1 if not found.
// Case-sensitive — proposal §2.3 makes member names (and so aliases)
// pure identifiers in [A-Za-z_][A-Za-z0-9_]* with case-sensitive match.
static int find_index(const char *name) {
    if (!name)
        return -1;
    for (size_t i = 0; i < g_count; i++) {
        if (g_table[i].name && strcmp(g_table[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

static int grow(size_t need) {
    if (need <= g_capacity)
        return 0;
    size_t cap = g_capacity ? g_capacity * 2 : 64;
    while (cap < need)
        cap *= 2;
    alias_entry_t *t = (alias_entry_t *)realloc(g_table, cap * sizeof(alias_entry_t));
    if (!t)
        return -1;
    memset(t + g_capacity, 0, (cap - g_capacity) * sizeof(alias_entry_t));
    g_table = t;
    g_capacity = cap;
    return 0;
}

static void free_entry(alias_entry_t *e) {
    if (!e)
        return;
    free(e->name);
    free(e->path);
    e->name = NULL;
    e->path = NULL;
}

static int validate_name(const char *name, char *err_buf, size_t err_size) {
    if (!object_validate_name(name, err_buf, err_size))
        return -1;
    return 0;
}

int alias_register_builtin(const char *name, const char *path, char *err_buf, size_t err_size) {
    if (!path) {
        set_err(err_buf, err_size, "null path for alias '$%s'", name ? name : "?");
        return -1;
    }
    if (validate_name(name, err_buf, err_size) < 0)
        return -1;

    int idx = find_index(name);
    if (idx >= 0) {
        alias_entry_t *e = &g_table[idx];
        if (e->kind == ALIAS_BUILTIN && e->path && strcmp(e->path, path) == 0)
            return 0; // idempotent re-registration with same target
        if (e->kind == ALIAS_BUILTIN) {
            set_err(err_buf, err_size, "built-in alias '%s' already maps to '%s'", name, e->path);
            return -1;
        }
        // Replacing a user alias with a built-in is fine — built-ins
        // win when both are registered (the framework registers them
        // before user aliases can be added).
        free_entry(e);
        e->name = xstrdup(name);
        e->path = xstrdup(path);
        e->kind = ALIAS_BUILTIN;
        return 0;
    }

    if (grow(g_count + 1) < 0) {
        set_err(err_buf, err_size, "out of memory");
        return -1;
    }
    alias_entry_t *e = &g_table[g_count++];
    e->name = xstrdup(name);
    e->path = xstrdup(path);
    e->kind = ALIAS_BUILTIN;
    return 0;
}

int alias_add_user(const char *name, const char *path, char *err_buf, size_t err_size) {
    if (!path) {
        set_err(err_buf, err_size, "null path for alias '$%s'", name ? name : "?");
        return -1;
    }
    if (validate_name(name, err_buf, err_size) < 0)
        return -1;

    int idx = find_index(name);
    if (idx >= 0) {
        alias_entry_t *e = &g_table[idx];
        if (e->kind == ALIAS_BUILTIN) {
            set_err(err_buf, err_size, "'%s' is a built-in alias", name);
            return -1;
        }
        // Replace existing user alias.
        free(e->path);
        e->path = xstrdup(path);
        return 0;
    }

    if (grow(g_count + 1) < 0) {
        set_err(err_buf, err_size, "out of memory");
        return -1;
    }
    alias_entry_t *e = &g_table[g_count++];
    e->name = xstrdup(name);
    e->path = xstrdup(path);
    e->kind = ALIAS_USER;
    return 0;
}

int alias_remove_user(const char *name, char *err_buf, size_t err_size) {
    int idx = find_index(name);
    if (idx < 0) {
        set_err(err_buf, err_size, "no such alias '%s'", name ? name : "(null)");
        return -1;
    }
    if (g_table[idx].kind == ALIAS_BUILTIN) {
        set_err(err_buf, err_size, "'%s' is a built-in alias and cannot be removed", name);
        return -1;
    }
    free_entry(&g_table[idx]);
    // Compact: move the last entry into this slot.
    if ((size_t)idx != g_count - 1)
        g_table[idx] = g_table[g_count - 1];
    memset(&g_table[g_count - 1], 0, sizeof(alias_entry_t));
    g_count--;
    return 0;
}

const char *alias_lookup(const char *name, alias_kind_t *kind_out) {
    int idx = find_index(name);
    if (idx < 0)
        return NULL;
    if (kind_out)
        *kind_out = g_table[idx].kind;
    return g_table[idx].path;
}

void alias_each(alias_iter_fn fn, void *ud) {
    if (!fn)
        return;
    for (size_t i = 0; i < g_count; i++) {
        if (!fn(g_table[i].name, g_table[i].path, g_table[i].kind, ud))
            return;
    }
}

size_t alias_count(void) {
    return g_count;
}

void alias_reset(void) {
    for (size_t i = 0; i < g_count; i++)
        free_entry(&g_table[i]);
    free(g_table);
    g_table = NULL;
    g_count = 0;
    g_capacity = 0;
}

void alias_clear_user(void) {
    // Compact in place: copy survivors forward.
    size_t w = 0;
    for (size_t r = 0; r < g_count; r++) {
        if (g_table[r].kind == ALIAS_BUILTIN) {
            if (w != r)
                g_table[w] = g_table[r];
            w++;
        } else {
            free_entry(&g_table[r]);
        }
    }
    // Zero the tail so we don't double-free if the table grows again.
    for (size_t i = w; i < g_count; i++)
        memset(&g_table[i], 0, sizeof(alias_entry_t));
    g_count = w;
}
