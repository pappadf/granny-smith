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

#include "job/job.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "object.h" // object_validate_name
#include "status.h"

LOG_USE_CATEGORY_NAME("alias");

typedef struct {
    char *name;
    char *path;
    alias_kind_t kind;
} alias_entry_t;

static alias_entry_t *g_table = NULL;
static size_t g_count = 0;
static size_t g_capacity = 0;

static void set_err(char *err_buf, size_t err_size, const char *fmt, ...) {
    if (!err_buf || !err_size)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_buf, err_size, fmt, ap);
    va_end(ap);
}

// Linear lookup. Returns the index in g_table or -1 if not found.
// Case-sensitive — member names (and so aliases) are
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

static status_t alias_register_builtin_impl(const char *name, const char *path, char *err_buf, size_t err_size) {
    if (!path) {
        set_err(err_buf, err_size, "null path for alias '$%s'", name ? name : "?");
        return STATUS_E_INVAL;
    }
    status_t rc = object_validate_name(name, err_buf, err_size);
    if (rc != STATUS_OK)
        return rc;

    int idx = find_index(name);
    if (idx >= 0) {
        alias_entry_t *e = &g_table[idx];
        if (e->kind == AK_BUILTIN && e->path && strcmp(e->path, path) == 0)
            return STATUS_OK; // idempotent re-registration with same target
        if (e->kind == AK_BUILTIN) {
            set_err(err_buf, err_size, "built-in alias '%s' already maps to '%s'", name, e->path);
            return STATUS_E_INVAL;
        }
        // Replacing a user alias with a built-in is fine — built-ins
        // win when both are registered (the framework registers them
        // before user aliases can be added). Log so a user who set
        // `$x = some.path` and then a subsystem registered `$x` as a
        // built-in to a different target can see what happened.
        LOG(1, "user alias '$%s' (→ %s) replaced by built-in (→ %s)", name, e->path ? e->path : "?", path);
        free_entry(e);
        e->name = strdup(name);
        e->path = strdup(path);
        e->kind = AK_BUILTIN;
        return STATUS_OK;
    }

    if (grow(g_count + 1) < 0) {
        set_err(err_buf, err_size, "out of memory");
        return STATUS_E_NOMEM;
    }
    alias_entry_t *e = &g_table[g_count++];
    e->name = strdup(name);
    e->path = strdup(path);
    e->kind = AK_BUILTIN;
    return STATUS_OK;
}

static status_t alias_add_user_impl(const char *name, const char *path, char *err_buf, size_t err_size) {
    if (!path) {
        set_err(err_buf, err_size, "null path for alias '$%s'", name ? name : "?");
        return STATUS_E_INVAL;
    }
    status_t rc = object_validate_name(name, err_buf, err_size);
    if (rc != STATUS_OK)
        return rc;

    int idx = find_index(name);
    if (idx >= 0) {
        alias_entry_t *e = &g_table[idx];
        if (e->kind == AK_BUILTIN) {
            set_err(err_buf, err_size, "'%s' is a built-in alias", name);
            return STATUS_E_INVAL;
        }
        // Replace existing user alias.
        free(e->path);
        e->path = strdup(path);
        return STATUS_OK;
    }

    if (grow(g_count + 1) < 0) {
        set_err(err_buf, err_size, "out of memory");
        return STATUS_E_NOMEM;
    }
    alias_entry_t *e = &g_table[g_count++];
    e->name = strdup(name);
    e->path = strdup(path);
    e->kind = AK_USER;
    return STATUS_OK;
}

static status_t alias_remove_user_impl(const char *name, char *err_buf, size_t err_size) {
    int idx = find_index(name);
    if (idx < 0) {
        set_err(err_buf, err_size, "no such alias '%s'", name ? name : "(null)");
        return STATUS_E_NOENT;
    }
    if (g_table[idx].kind == AK_BUILTIN) {
        set_err(err_buf, err_size, "'%s' is a built-in alias and cannot be removed", name);
        return STATUS_E_INVAL;
    }
    free_entry(&g_table[idx]);
    // Close the gap by shifting the tail down one slot, keeping the
    // registration order alias_each promises (removal is a cold path).
    memmove(&g_table[idx], &g_table[idx + 1], (g_count - 1 - (size_t)idx) * sizeof(alias_entry_t));
    memset(&g_table[g_count - 1], 0, sizeof(alias_entry_t));
    g_count--;
    return STATUS_OK;
}

static const char *alias_lookup_impl(const char *name, alias_kind_t *kind_out) {
    int idx = find_index(name);
    if (idx < 0)
        return NULL;
    if (kind_out)
        *kind_out = g_table[idx].kind;
    return g_table[idx].path;
}

static void alias_each_impl(alias_iter_fn fn, void *ud) {
    if (!fn)
        return;
    for (size_t i = 0; i < g_count; i++) {
        if (!fn(g_table[i].name, g_table[i].path, g_table[i].kind, ud))
            return;
    }
}

static size_t alias_count_impl(void) {
    return g_count;
}

static void alias_reset_impl(void) {
    for (size_t i = 0; i < g_count; i++)
        free_entry(&g_table[i]);
    free(g_table);
    g_table = NULL;
    g_count = 0;
    g_capacity = 0;
}

static void alias_clear_user_impl(void) {
    // Compact in place: copy survivors forward.
    size_t w = 0;
    for (size_t r = 0; r < g_count; r++) {
        if (g_table[r].kind == AK_BUILTIN) {
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

// === The table lock (job/job.h): every public entry takes it for the one
// operation; the interpreter on the job thread and the emulator thread
// (breakpoint conditions, completion, shell.vars) both read here. =========

status_t alias_register_builtin(const char *name, const char *path, char *err_buf, size_t err_size) {
    job_tables_lock();
    status_t r = alias_register_builtin_impl(name, path, err_buf, err_size);
    job_tables_unlock();
    return r;
}

status_t alias_add_user(const char *name, const char *path, char *err_buf, size_t err_size) {
    job_tables_lock();
    status_t r = alias_add_user_impl(name, path, err_buf, err_size);
    job_tables_unlock();
    return r;
}

status_t alias_remove_user(const char *name, char *err_buf, size_t err_size) {
    job_tables_lock();
    status_t r = alias_remove_user_impl(name, err_buf, err_size);
    job_tables_unlock();
    return r;
}

const char *alias_lookup(const char *name, alias_kind_t *kind_out) {
    job_tables_lock();
    const char *r = alias_lookup_impl(name, kind_out);
    job_tables_unlock();
    return r;
}

bool alias_lookup_copy(const char *name, char *buf, size_t size, alias_kind_t *kind_out) {
    job_tables_lock();
    const char *r = alias_lookup_impl(name, kind_out);
    bool ok = r && buf && strlen(r) < size;
    if (ok)
        memcpy(buf, r, strlen(r) + 1);
    job_tables_unlock();
    return ok;
}

void alias_each(alias_iter_fn fn, void *ud) {
    job_tables_lock();
    alias_each_impl(fn, ud);
    job_tables_unlock();
}

size_t alias_count(void) {
    job_tables_lock();
    size_t r = alias_count_impl();
    job_tables_unlock();
    return r;
}

void alias_reset(void) {
    job_tables_lock();
    alias_reset_impl();
    job_tables_unlock();
}

void alias_clear_user(void) {
    job_tables_lock();
    alias_clear_user_impl();
    job_tables_unlock();
}
