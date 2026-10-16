// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell_var.c
// Scoped shell binding store. See shell_var.h for the contract.
//
// Storage: a fixed stack of scopes, each holding a small dynamic array
// of heap-allocated binding entries (stable pointers, so the object
// invalidator hooks can point into an entry safely across growth).
// Scope 0 is the process-global scope (--var bindings, TMP_DIR); scope
// 1 is the script/session top level; function calls push above that.
// The built-in/user alias table (alias.c) is the read-only fallback
// behind every scope — its entries surface as VK_REF values, which is
// exactly the reference-binding semantics.
//
// Lookup is a linear scan of each scope: scopes hold a handful to a few
// tens of bindings, where a scan is as fast as a hash and keeps creation
// order for free.  Revisit if scripts start keeping hundreds.

#include "shell_var.h"

#include "alias.h"
#include "object.h"
#include "status.h"
#include "value.h"

#include "job/job.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Function frames on top of the two fixed scopes (16-frame cap).
#define SCOPE_FRAMES_MAX 16
#define SCOPE_MAX        (2 + SCOPE_FRAMES_MAX)

// One binding. Heap-allocated so its address is stable (the object
// invalidator callbacks point into its watch).
typedef struct binding {
    char *name;
    value_t value;
    // Every object the value holds, at any depth (a VK_OBJECT, or one in a
    // list or map); watch.gone means one was destroyed and reads yield
    // VK_ERROR instead of handing out a dangling pointer.
    object_watch_t watch;
} binding_t;

typedef struct scope {
    binding_t **items;
    int n;
    int cap;
} scope_t;

static scope_t g_scopes[SCOPE_MAX];
static int g_n_scopes = 0;

/* --- entry lifecycle ------------------------------------------------------ */

// Stop watching and free the stored value.
static void binding_clear_value(binding_t *b) {
    object_unwatch(&b->watch);
    value_free(&b->value);
}

// Store a value into an entry, arming the staleness watch over every object
// it holds.  False (value freed, binding left empty) on out-of-memory: an
// unwatched object could dangle after machine.boot.
static bool binding_store(binding_t *b, value_t v) {
    binding_clear_value(b);
    if (!object_watch_value(&b->watch, &v)) {
        value_free(&v);
        return false;
    }
    b->value = v;
    return true;
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

// Remove (and free) b, keeping the remaining bindings in creation order
static void scope_remove(scope_t *s, binding_t *b) {
    for (int i = 0; i < s->n; i++) {
        if (s->items[i] == b) {
            binding_free(b);
            memmove(&s->items[i], &s->items[i + 1], (size_t)(s->n - i - 1) * sizeof(s->items[0]));
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

static value_t shell_binding_get_impl(const char *name) {
    if (!name || !*name)
        return val_err("no such binding '$'");
    binding_t *b = find_binding(name);
    if (b) {
        if (b->watch.gone)
            return val_err("'$%s' holds an object that was destroyed (e.g. by machine.boot)", name);
        return value_dup(&b->value);
    }
    // Alias-table fallback: aliases are reference bindings.
    const char *path = alias_lookup(name, NULL);
    if (path)
        return val_ref(path);
    return val_err("no such binding '$%s'", name);
}

static int shell_binding_let_impl(const char *name, value_t v, char *err_buf, size_t err_size) {
    char err[160];
    if (object_validate_name(name, err, sizeof(err)) != STATUS_OK) {
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
    if (!binding_store(b, v)) {
        if (err_buf && err_size)
            snprintf(err_buf, err_size, "let: out of memory");
        return -1;
    }
    return 0;
}

static int shell_binding_mutate_impl(const char *name, value_t v) {
    binding_t *b = find_binding(name);
    if (!b) {
        value_free(&v);
        return -1;
    }
    return binding_store(b, v) ? 0 : -1;
}

static shell_binding_kind_t shell_binding_classify_impl(const char *name, const value_t **value_out,
                                                        const char **alias_path_out) {
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

static int shell_binding_push_scope_impl(void) {
    if (g_n_scopes >= SCOPE_MAX)
        return -1;
    memset(&g_scopes[g_n_scopes], 0, sizeof(scope_t));
    g_n_scopes++;
    return 0;
}

static void shell_binding_pop_scope_impl(void) {
    if (g_n_scopes <= 2)
        return; // never pop the global / top-level scopes
    scope_t *s = &g_scopes[--g_n_scopes];
    for (int i = 0; i < s->n; i++)
        binding_free(s->items[i]);
    free(s->items);
    memset(s, 0, sizeof(*s));
}

static binding_t *shell_binding_detach_top_impl(const char *name) {
    scope_t *top = &g_scopes[g_n_scopes - 1];
    binding_t *b = scope_find(top, name);
    if (!b)
        return NULL;
    // Unlink without freeing: the entry (and its invalidator) lives on.  The
    // others keep their order (listings and completion follow it).
    for (int i = 0; i < top->n; i++) {
        if (top->items[i] == b) {
            memmove(&top->items[i], &top->items[i + 1], (size_t)(top->n - i - 1) * sizeof(top->items[0]));
            top->n--;
            break;
        }
    }
    return b;
}

static void shell_binding_reattach_top_impl(binding_t *b) {
    if (!b)
        return;
    scope_t *top = &g_scopes[g_n_scopes - 1];
    binding_t *cur = scope_find(top, b->name);
    if (cur)
        scope_remove(top, cur);
    // Room for the entry; on OOM the shadowed binding is lost, not leaked
    if (top->n == top->cap) {
        int cap = top->cap ? top->cap * 2 : 16;
        binding_t **t = (binding_t **)realloc(top->items, (size_t)cap * sizeof(*t));
        if (!t) {
            binding_free(b);
            return;
        }
        top->items = t;
        top->cap = cap;
    }
    top->items[top->n++] = b;
}

static void shell_binding_remove_top_impl(const char *name) {
    scope_t *top = &g_scopes[g_n_scopes - 1];
    binding_t *b = scope_find(top, name);
    if (b)
        scope_remove(top, b);
}

/* --- legacy string API ---------------------------------------------------- */

static int shell_var_set_impl(const char *name, const char *value, char *err_buf, size_t err_size) {
    // The same rule as `let`: a name `$name` could never read is refused
    char err[160];
    if (!value || object_validate_name(name, err, sizeof(err)) != STATUS_OK) {
        if (err_buf && err_size)
            snprintf(err_buf, err_size, "%s", value ? err : "no value");
        return -1;
    }
    scope_t *globals = &g_scopes[0];
    binding_t *b = scope_find(globals, name);
    if (!b)
        b = scope_add(globals, name);
    if (!b || !binding_store(b, val_str(value))) {
        if (err_buf && err_size)
            snprintf(err_buf, err_size, "out of memory");
        return -1;
    }
    return 0;
}

static const char *shell_var_get_impl(const char *name) {
    binding_t *b = name ? find_binding(name) : NULL;
    if (!b || b->value.kind != VK_STRING)
        return NULL;
    return b->value.s;
}

static void shell_var_each_impl(shell_var_iter_fn fn, void *ud) {
    if (!fn)
        return;
    for (int i = g_n_scopes - 1; i >= 0; i--) {
        scope_t *s = &g_scopes[i];
        for (int k = 0; k < s->n; k++) {
            binding_t *b = s->items[k];
            if (!b->watch.gone) {
                if (!fn(b->name, &b->value, ud))
                    return;
                continue;
            }
            // A stale value holds dangling object pointers: hand out the
            // error a read would give, never the value itself
            value_t e = val_err("'$%s' holds an object that was destroyed", b->name);
            bool more = fn(b->name, &e, ud);
            value_free(&e);
            if (!more)
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
    // Relative on purpose (see shell_var.h): the project's tmp/ scratch
    // directory when headless runs from the repository root, as AGENTS.md
    // asks of scratch files, and /tmp on WASM, whose working directory is /.
    shell_var_set("TMP_DIR", "tmp", NULL, 0);
}

// === The table lock (job/job.h): every public entry takes it for the one
// operation; the interpreter on the job thread and the emulator thread
// (breakpoint conditions, completion, shell.vars) both read here. =========

value_t shell_binding_get(const char *name) {
    job_tables_lock();
    value_t r = shell_binding_get_impl(name);
    job_tables_unlock();
    return r;
}

int shell_binding_let(const char *name, value_t v, char *err_buf, size_t err_size) {
    job_tables_lock();
    int r = shell_binding_let_impl(name, v, err_buf, err_size);
    job_tables_unlock();
    return r;
}

int shell_binding_mutate(const char *name, value_t v) {
    job_tables_lock();
    int r = shell_binding_mutate_impl(name, v);
    job_tables_unlock();
    return r;
}

shell_binding_kind_t shell_binding_classify(const char *name, const value_t **value_out, const char **alias_path_out) {
    job_tables_lock();
    shell_binding_kind_t r = shell_binding_classify_impl(name, value_out, alias_path_out);
    job_tables_unlock();
    return r;
}

int shell_binding_push_scope(void) {
    job_tables_lock();
    int r = shell_binding_push_scope_impl();
    job_tables_unlock();
    return r;
}

void shell_binding_pop_scope(void) {
    job_tables_lock();
    shell_binding_pop_scope_impl();
    job_tables_unlock();
}

struct binding *shell_binding_detach_top(const char *name) {
    job_tables_lock();
    binding_t *r = shell_binding_detach_top_impl(name);
    job_tables_unlock();
    return r;
}

void shell_binding_reattach_top(struct binding *b) {
    job_tables_lock();
    shell_binding_reattach_top_impl(b);
    job_tables_unlock();
}

void shell_binding_remove_top(const char *name) {
    job_tables_lock();
    shell_binding_remove_top_impl(name);
    job_tables_unlock();
}

int shell_var_set(const char *name, const char *value, char *err_buf, size_t err_size) {
    job_tables_lock();
    int r = shell_var_set_impl(name, value, err_buf, err_size);
    job_tables_unlock();
    return r;
}

const char *shell_var_get(const char *name) {
    job_tables_lock();
    const char *r = shell_var_get_impl(name);
    job_tables_unlock();
    return r;
}

void shell_var_each(shell_var_iter_fn fn, void *ud) {
    job_tables_lock();
    shell_var_each_impl(fn, ud);
    job_tables_unlock();
}
