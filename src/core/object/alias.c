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
#include "value.h"

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

// === Object-model class descriptor =========================================
//
// `shell.alias` exposes alias add / remove / list as object methods.

static value_t method_alias_add(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2)
        return val_err("shell.alias.add: expected 2 args (name, path)");
    if (argv[0].kind != V_STRING || argv[1].kind != V_STRING)
        return val_err("shell.alias.add: name and path must be strings");
    char err[160];
    if (alias_add_user(argv[0].s, argv[1].s, err, sizeof(err)) < 0)
        return val_err("%s", err);
    return val_none();
}

static value_t method_alias_remove(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("shell.alias.remove: expected name (string)");
    char err[160];
    if (alias_remove_user(argv[0].s, err, sizeof(err)) < 0)
        return val_err("%s", err);
    return val_none();
}

// shell.alias.list builds a V_LIST of V_STRING entries: each "name=path".
typedef struct {
    value_t *items;
    size_t len;
    size_t cap;
} list_acc_t;

static bool list_acc_collect(const char *name, const char *path, alias_kind_t kind, void *ud) {
    list_acc_t *acc = (list_acc_t *)ud;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s=%s%s", name, path, kind == ALIAS_BUILTIN ? " (built-in)" : "");
    if (acc->len + 1 > acc->cap) {
        size_t cap = acc->cap ? acc->cap * 2 : 32;
        value_t *t = (value_t *)realloc(acc->items, cap * sizeof(value_t));
        if (!t)
            return false;
        acc->items = t;
        acc->cap = cap;
    }
    acc->items[acc->len++] = val_str(buf);
    return true;
}

static value_t method_alias_list(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    list_acc_t acc = {0};
    alias_each(list_acc_collect, &acc);
    return val_list(acc.items, acc.len);
}

static const arg_decl_t alias_add_args[] = {
    {.name = "name", .kind = V_STRING, .flags = 0, .doc = "alias identifier (no $)"          },
    {.name = "path", .kind = V_STRING, .flags = 0, .doc = "object path the alias substitutes"},
};
static const arg_decl_t alias_remove_args[] = {
    {.name = "name", .kind = V_STRING, .flags = 0, .doc = "alias identifier (no $)"},
};

static const member_t shell_alias_members[] = {
    {.kind = M_METHOD,
     .name = "add",
     .doc = "Register a user alias",
     .flags = 0,
     .method = {.args = alias_add_args, .nargs = 2, .result = V_NONE, .fn = method_alias_add}      },
    {.kind = M_METHOD,
     .name = "remove",
     .doc = "Remove a user alias",
     .flags = 0,
     .method = {.args = alias_remove_args, .nargs = 1, .result = V_NONE, .fn = method_alias_remove}},
    {.kind = M_METHOD,
     .name = "list",
     .doc = "List aliases as 'name=path' strings",
     .flags = 0,
     .method = {.args = NULL, .nargs = 0, .result = V_LIST, .fn = method_alias_list}               },
};

const class_desc_t shell_alias_class = {
    .name = "alias",
    .members = shell_alias_members,
    .n_members = sizeof(shell_alias_members) / sizeof(shell_alias_members[0]),
};
