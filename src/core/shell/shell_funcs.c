// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell_funcs.c
// User-defined script functions. See shell_funcs.h.
//
// Each function is one registry entry plus one attached entry object
// under `shell.functions`, so `shell.functions.NAME` resolves for
// introspection (`name`, `params` attributes) and removal (`remove()`).
// Flat-name call resolution happens in script.c (command form) and via
// the expr function hook (call form in expressions and templates).

#include "shell_funcs.h"

#include "expr.h"
#include "script.h"
#include "shell_var.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct script_func {
    char *name;
    char **params;
    int n_params;
    script_block_t *body;
    struct object *entry_obj; // attached under shell.functions
    struct script_func *next;
};

static script_func_t *g_funcs = NULL;
static struct object *g_functions_obj = NULL; // the shell.functions container

// === Entry class ============================================================

static const class_desc_t func_entry_class;

static script_func_t *func_from(struct object *self) {
    return (script_func_t *)object_data(self);
}

static value_t func_get_name(struct object *self, const member_t *m) {
    (void)m;
    script_func_t *f = func_from(self);
    return val_str(f ? f->name : "");
}

// `params` — the declared parameter list, comma-joined.
static value_t func_get_params(struct object *self, const member_t *m) {
    (void)m;
    script_func_t *f = func_from(self);
    char buf[256] = "";
    size_t off = 0;
    for (int i = 0; f && i < f->n_params; i++) {
        int n = snprintf(buf + off, sizeof(buf) - off, "%s%s", i ? ", " : "", f->params[i]);
        if (n < 0 || (size_t)n >= sizeof(buf) - off)
            break;
        off += (size_t)n;
    }
    return val_str(buf);
}

static value_t func_method_remove(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    script_func_t *f = func_from(self);
    if (!f)
        return val_err("function entry has no registry backing");
    if (shell_func_remove(f->name) < 0)
        return val_err("no such function");
    return val_none();
}

static const member_t func_entry_members[] = {
    {.kind = M_ATTR,
     .name = "name",
     .doc = "Function name",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = func_get_name, .set = NULL}},
    {.kind = M_ATTR,
     .name = "params",
     .doc = "Declared parameter list",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = func_get_params, .set = NULL}},
    {.kind = M_METHOD,
     .name = "remove",
     .doc = "Remove this function",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = func_method_remove}},
};

static const class_desc_t func_entry_class = {
    .name = "function",
    .members = func_entry_members,
    .n_members = sizeof(func_entry_members) / sizeof(func_entry_members[0]),
};

// `shell.functions` container: a plain namespace object; entries are
// attached children so path resolution finds them by name.
static const class_desc_t functions_class = {
    .name = "functions",
    .members = NULL,
    .n_members = 0,
};

// === Registry ===============================================================

static void func_free(script_func_t *f) {
    if (!f)
        return;
    if (f->entry_obj) {
        object_detach(f->entry_obj);
        object_delete(f->entry_obj);
    }
    for (int i = 0; i < f->n_params; i++)
        free(f->params[i]);
    free(f->params);
    script_block_free(f->body);
    free(f->name);
    free(f);
}

script_func_t *shell_func_find(const char *name) {
    for (script_func_t *f = g_funcs; f; f = f->next)
        if (strcmp(f->name, name) == 0)
            return f;
    return NULL;
}

int shell_func_remove(const char *name) {
    script_func_t **pp = &g_funcs;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            script_func_t *f = *pp;
            *pp = f->next;
            func_free(f);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

int shell_func_define(const char *name, char **params, int n_params, script_block_t *body, char *err_buf,
                      size_t err_size) {
    char err[160];
    if (!body) {
        snprintf(err_buf, err_size, "function body was already consumed (def re-executed?)");
        return -1;
    }
    if (!object_validate_name(name, err, sizeof(err))) {
        snprintf(err_buf, err_size, "%s", err);
        script_block_free(body);
        return -1;
    }
    // Duplicate parameter names are a definition error.
    for (int i = 0; i < n_params; i++) {
        if (!object_validate_name(params[i], err, sizeof(err))) {
            snprintf(err_buf, err_size, "parameter %d: %s", i + 1, err);
            script_block_free(body);
            return -1;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(params[i], params[j]) == 0) {
                snprintf(err_buf, err_size, "duplicate parameter '%s'", params[i]);
                script_block_free(body);
                return -1;
            }
        }
    }

    // Redefinition replaces.
    shell_func_remove(name);

    script_func_t *f = (script_func_t *)calloc(1, sizeof(*f));
    if (!f) {
        snprintf(err_buf, err_size, "out of memory");
        script_block_free(body);
        return -1;
    }
    f->name = strdup(name);
    f->n_params = n_params;
    f->params = n_params > 0 ? (char **)calloc((size_t)n_params, sizeof(char *)) : NULL;
    for (int i = 0; i < n_params; i++)
        f->params[i] = strdup(params[i]);
    f->body = body;
    f->next = g_funcs;
    g_funcs = f;

    if (g_functions_obj) {
        f->entry_obj = object_new(&func_entry_class, f, f->name);
        if (f->entry_obj)
            object_attach(g_functions_obj, f->entry_obj);
    }
    return 0;
}

value_t shell_func_call(script_func_t *f, int argc, const value_t *argv, int named_n, const named_arg_t *named) {
    if (!f)
        return val_err("no such function");
    if (argc > f->n_params)
        return val_err("%s: too many arguments (got %d, want %d)", f->name, argc, f->n_params);

    // Bind: positionals left-to-right, then named by parameter name.
    const value_t *bound[16] = {0};
    if (f->n_params > 16)
        return val_err("%s: too many parameters", f->name);
    for (int i = 0; i < argc; i++)
        bound[i] = &argv[i];
    for (int i = 0; i < named_n; i++) {
        int slot = -1;
        for (int k = 0; k < f->n_params; k++) {
            if (strcmp(named[i].name, f->params[k]) == 0) {
                slot = k;
                break;
            }
        }
        if (slot < 0)
            return val_err("%s: unknown argument '%s'", f->name, named[i].name);
        if (bound[slot])
            return val_err("%s: duplicate argument '%s'", f->name, named[i].name);
        bound[slot] = &named[i].value;
    }
    for (int i = 0; i < f->n_params; i++) {
        if (!bound[i])
            return val_err("%s: missing argument '%s'", f->name, f->params[i]);
    }

    if (shell_binding_push_scope() < 0)
        return val_err("%s: recursion too deep (16-frame cap)", f->name);
    char err[160];
    for (int i = 0; i < f->n_params; i++) {
        if (shell_binding_let(f->params[i], value_dup(bound[i]), err, sizeof(err)) < 0) {
            shell_binding_pop_scope();
            return val_err("%s: %s", f->name, err);
        }
    }
    value_t r = script_exec_func_body(f->body);
    shell_binding_pop_scope();
    if (val_is_error(&r)) {
        value_t wrapped = val_err("%s: %s", f->name, r.err ? r.err : "failed");
        value_free(&r);
        return wrapped;
    }
    return r;
}

// === Expression-layer hook ==================================================

static value_t func_expr_hook(void *ud, const char *name, int argc, const value_t *argv, int named_n,
                              const named_arg_t *named) {
    (void)ud;
    script_func_t *f = shell_func_find(name);
    if (!f)
        return val_err("no such function '%s'", name);
    return shell_func_call(f, argc, argv, named_n, named);
}

// === Install / uninstall ====================================================

void shell_funcs_install(struct object *shell_obj) {
    if (!shell_obj || g_functions_obj)
        return;
    g_functions_obj = object_new(&functions_class, NULL, "functions");
    if (g_functions_obj)
        object_attach(shell_obj, g_functions_obj);
    // (Re-)attach entry objects for functions that survived a reinstall.
    for (script_func_t *f = g_funcs; f; f = f->next) {
        if (!f->entry_obj && g_functions_obj) {
            f->entry_obj = object_new(&func_entry_class, f, f->name);
            if (f->entry_obj)
                object_attach(g_functions_obj, f->entry_obj);
        }
    }
    expr_set_func_hook(func_expr_hook, NULL);
}

void shell_funcs_uninstall(void) {
    // Detach entry objects (the container object is torn down with the
    // shell stub by root_uninstall); keep the function definitions —
    // like bindings, they are process state, not machine state.
    for (script_func_t *f = g_funcs; f; f = f->next) {
        if (f->entry_obj) {
            object_detach(f->entry_obj);
            object_delete(f->entry_obj);
            f->entry_obj = NULL;
        }
    }
    if (g_functions_obj) {
        object_detach(g_functions_obj);
        object_delete(g_functions_obj);
        g_functions_obj = NULL;
    }
}
