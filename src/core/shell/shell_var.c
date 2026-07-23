// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell_var.c
// Scoped shell binding store. See shell_var.h for the contract.
//
// Storage: a fixed stack of scopes, each holding a small dynamic array
// of heap-allocated binding entries (stable pointers, so the V_OBJECT
// invalidator hook can carry an entry pointer safely across growth).
// Scope 0 is the process-global scope (--var bindings, TMP_DIR); scope
// 1 is the script/session top level; function calls push above that.
// The built-in/user alias table (alias.c) is the read-only fallback
// behind every scope — its entries surface as V_REF values, which is
// exactly the reference-binding semantics of §3.5.

#include "shell_var.h"

#include "alias.h"
#include "object.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Function frames on top of the two fixed scopes (§3.7: 16-frame cap).
#define SCOPE_FRAMES_MAX 16
#define SCOPE_MAX        (2 + SCOPE_FRAMES_MAX)

// One binding. Heap-allocated so its address is stable (the object
// invalidator callback holds it as user-data).
typedef struct binding {
    char *name;
    value_t value;
    struct object *watched; // non-NULL while value is a live V_OBJECT
    bool stale; // watched object was destroyed; reads yield V_ERROR
} binding_t;

typedef struct scope {
    binding_t **items;
    int n;
    int cap;
} scope_t;

static scope_t g_scopes[SCOPE_MAX];
static int g_n_scopes = 0;

/* --- entry lifecycle ------------------------------------------------------ */

// Invalidator: the watched object is being destroyed. Mark the binding
// stale; its next read reports the death instead of dereferencing a
// dangling pointer.
static void binding_object_gone(void *ud) {
    binding_t *b = (binding_t *)ud;
    b->stale = true;
    b->watched = NULL;
}

// Stop watching (if we are) and free the stored value.
static void binding_clear_value(binding_t *b) {
    if (b->watched) {
        object_unregister_invalidator(b->watched, binding_object_gone, b);
        b->watched = NULL;
    }
    b->stale = false;
    value_free(&b->value);
}

// Store a value into an entry, arming the staleness watch for V_OBJECT.
static void binding_store(binding_t *b, value_t v) {
    binding_clear_value(b);
    b->value = v;
    if (v.kind == V_OBJECT && v.obj) {
        b->watched = v.obj;
        object_register_invalidator(v.obj, binding_object_gone, b);
    }
}

static void binding_free(binding_t *b) {
    if (!b)
        return;
    binding_clear_value(b);
    free(b->name);
    free(b);
}

/* --- scope helpers -------------------------------------------------------- */

static binding_t *scope_find(scope_t *s, const char *name) {
    for (int i = 0; i < s->n; i++) {
        if (s->items[i]->name && strcmp(s->items[i]->name, name) == 0)
            return s->items[i];
    }
    return NULL;
}

static binding_t *scope_add(scope_t *s, const char *name) {
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 16;
        binding_t **t = (binding_t **)realloc(s->items, (size_t)cap * sizeof(*t));
        if (!t)
            return NULL;
        s->items = t;
        s->cap = cap;
    }
    binding_t *b = (binding_t *)calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->name = strdup(name);
    if (!b->name) {
        free(b);
        return NULL;
    }
    s->items[s->n++] = b;
    return b;
}

static void scope_remove(scope_t *s, binding_t *b) {
    for (int i = 0; i < s->n; i++) {
        if (s->items[i] == b) {
            binding_free(b);
            s->items[i] = s->items[s->n - 1];
            s->n--;
            return;
        }
    }
}

// Find a binding walking scopes top-down. Returns NULL if absent.
static binding_t *find_binding(const char *name) {
    for (int i = g_n_scopes - 1; i >= 0; i--) {
        binding_t *b = scope_find(&g_scopes[i], name);
        if (b)
            return b;
    }
    return NULL;
}

/* --- public API ----------------------------------------------------------- */

value_t shell_binding_get(const char *name) {
    if (!name || !*name)
        return val_err("no such binding '$'");
    binding_t *b = find_binding(name);
    if (b) {
        if (b->stale)
            return val_err("'$%s' holds an object that was destroyed (e.g. by machine.boot)", name);
        return value_dup(&b->value);
    }
    // Alias-table fallback: aliases are reference bindings (§3.5).
    const char *path = alias_lookup(name, NULL);
    if (path)
        return val_ref(path);
    return val_err("no such binding '$%s'", name);
}

int shell_binding_let(const char *name, value_t v, char *err_buf, size_t err_size) {
    char err[160];
    if (!object_validate_name(name, err, sizeof(err))) {
        if (err_buf && err_size)
            snprintf(err_buf, err_size, "let: %s", err);
        value_free(&v);
        return -1;
    }
    scope_t *top = &g_scopes[g_n_scopes - 1];
    binding_t *b = scope_find(top, name);
    if (!b)
        b = scope_add(top, name);
    if (!b) {
        if (err_buf && err_size)
            snprintf(err_buf, err_size, "let: out of memory");
        value_free(&v);
        return -1;
    }
    binding_store(b, v);
    return 0;
}

int shell_binding_mutate(const char *name, value_t v) {
    binding_t *b = find_binding(name);
    if (!b) {
        value_free(&v);
        return -1;
    }
    binding_store(b, v);
    return 0;
}

shell_binding_kind_t shell_binding_classify(const char *name, const value_t **value_out, const char **alias_path_out) {
    if (value_out)
        *value_out = NULL;
    if (alias_path_out)
        *alias_path_out = NULL;
    binding_t *b = find_binding(name);
    if (b) {
        if (value_out)
            *value_out = &b->value;
        return SHELL_BINDING_VALUE;
    }
    const char *path = alias_lookup(name, NULL);
    if (path) {
        if (alias_path_out)
            *alias_path_out = path;
        return SHELL_BINDING_ALIAS;
    }
    return SHELL_BINDING_NONE;
}

int shell_binding_push_scope(void) {
    if (g_n_scopes >= SCOPE_MAX)
        return -1;
    memset(&g_scopes[g_n_scopes], 0, sizeof(scope_t));
    g_n_scopes++;
    return 0;
}

void shell_binding_pop_scope(void) {
    if (g_n_scopes <= 2)
        return; // never pop the global / top-level scopes
    scope_t *s = &g_scopes[--g_n_scopes];
    for (int i = 0; i < s->n; i++)
        binding_free(s->items[i]);
    free(s->items);
    memset(s, 0, sizeof(*s));
}

bool shell_binding_save_top(const char *name, value_t *saved_out) {
    scope_t *top = &g_scopes[g_n_scopes - 1];
    binding_t *b = scope_find(top, name);
    if (!b)
        return false;
    if (saved_out)
        *saved_out = b->stale ? val_err("'$%s' holds a destroyed object", name) : value_dup(&b->value);
    return true;
}

void shell_binding_remove_top(const char *name) {
    scope_t *top = &g_scopes[g_n_scopes - 1];
    binding_t *b = scope_find(top, name);
    if (b)
        scope_remove(top, b);
}

/* --- legacy string API ---------------------------------------------------- */

int shell_var_set(const char *name, const char *value) {
    if (!name || !*name || !value)
        return -1;
    scope_t *globals = &g_scopes[0];
    binding_t *b = scope_find(globals, name);
    if (!b)
        b = scope_add(globals, name);
    if (!b)
        return -1;
    binding_store(b, val_str(value));
    return 0;
}

const char *shell_var_get(const char *name) {
    binding_t *b = name ? find_binding(name) : NULL;
    if (!b || b->value.kind != V_STRING)
        return NULL;
    return b->value.s;
}

void shell_var_each(shell_var_iter_fn fn, void *ud) {
    if (!fn)
        return;
    for (int i = g_n_scopes - 1; i >= 0; i--) {
        scope_t *s = &g_scopes[i];
        for (int k = 0; k < s->n; k++) {
            if (!fn(s->items[k]->name, &s->items[k]->value, ud))
                return;
        }
    }
}

/* --- init ---------------------------------------------------------------- */

void shell_var_init(void) {
    if (g_n_scopes == 0) {
        memset(g_scopes, 0, sizeof(g_scopes));
        g_n_scopes = 2; // [0] process globals, [1] script/session top level
    }
    shell_var_set("TMP_DIR", "tmp");
}
