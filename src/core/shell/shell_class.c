// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell_class.c
// The `Shell` class on the object root — the last subsystem to enter
// the object model. After this lands, the JS bridge's free-form-line
// kind (pending=4) retires; every JS→C call rides on `gs_eval` (kind=1),
// either against typed paths (`cpu.pc`) or against the shell's own
// methods (`shell.run`, `shell.complete`, `shell.expand`, …). See
// proposal-shell-as-object-model-citizen.md.
//
// The Shell class is a thin wrapper. Its method bodies forward to the
// existing shell internals (the static `dispatch_command` in shell.c
// reached via shell_internal.h, `shell_complete`, `shell_var_expand`,
// the alias-table API). No business logic moves here.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alias.h"
#include "cmd_complete.h"
#include "cmd_types.h"
#include "object.h"
#include "scheduler.h"
#include "shell.h"
#include "shell_internal.h"
#include "shell_var.h"
#include "system.h"
#include "value.h"

// === Attribute getters ====================================================

// `shell.prompt` — current prompt text. Read once per terminal redraw.
static value_t shell_get_prompt(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    char buf[256];
    shell_build_prompt(buf, sizeof(buf));
    return val_str(buf);
}

// `shell.running` — true while the scheduler is running. The proposal
// frames this as "true while a command is in flight"; in practice the
// only commands that meaningfully run are scheduler-driven (the rest
// finish synchronously), so this is the right proxy.
static value_t shell_get_running(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    scheduler_t *s = system_scheduler();
    return val_bool(s ? scheduler_is_running(s) : false);
}

// `shell.aliases` — list of "name=path" strings. Same shape as the
// existing `shell.alias.list` method; kept here so callers can read the
// attribute without invoking a method.
typedef struct {
    value_t *items;
    size_t len;
    size_t cap;
} str_list_t;

static bool str_list_push(str_list_t *acc, const char *s) {
    if (!s)
        return true;
    if (acc->len + 1 > acc->cap) {
        size_t cap = acc->cap ? acc->cap * 2 : 16;
        value_t *t = (value_t *)realloc(acc->items, cap * sizeof(value_t));
        if (!t)
            return false;
        acc->items = t;
        acc->cap = cap;
    }
    acc->items[acc->len++] = val_str(s);
    return true;
}

static bool alias_collect_cb(const char *name, const char *path, alias_kind_t kind, void *ud) {
    str_list_t *acc = (str_list_t *)ud;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s=%s%s", name, path, kind == ALIAS_BUILTIN ? " (built-in)" : "");
    return str_list_push(acc, buf);
}

static value_t shell_get_aliases(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    str_list_t acc = {0};
    alias_each(alias_collect_cb, &acc);
    return val_list(acc.items, acc.len);
}

// `shell.vars` — list of "name=value" strings. Iteration walks the
// internal table; for V_STRING entries the value is rendered verbatim,
// other kinds emit their JSON-ish formatter shape.
static void format_value_compact(const value_t *v, char *buf, size_t buf_size) {
    if (!v) {
        snprintf(buf, buf_size, "");
        return;
    }
    switch (v->kind) {
    case V_STRING:
        snprintf(buf, buf_size, "%s", v->s ? v->s : "");
        break;
    case V_BOOL:
        snprintf(buf, buf_size, "%s", v->b ? "true" : "false");
        break;
    case V_INT:
        snprintf(buf, buf_size, "%lld", (long long)v->i);
        break;
    case V_UINT:
        snprintf(buf, buf_size, "%llu", (unsigned long long)v->u);
        break;
    case V_FLOAT:
        snprintf(buf, buf_size, "%g", v->f);
        break;
    default:
        snprintf(buf, buf_size, "<%d>", (int)v->kind);
        break;
    }
}

// The variable table is private to shell_var.c; iterate via the
// shell_var_each() callback the module exposes.
static bool var_collect_cb(const char *name, const value_t *v, void *ud) {
    str_list_t *acc = (str_list_t *)ud;
    char val_buf[160];
    format_value_compact(v, val_buf, sizeof(val_buf));
    char line[256];
    snprintf(line, sizeof(line), "%s=%s", name, val_buf);
    return str_list_push(acc, line);
}

static value_t shell_get_vars(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    str_list_t acc = {0};
    shell_var_each(var_collect_cb, &acc);
    return val_list(acc.items, acc.len);
}

// === Method bodies ========================================================

// `shell.run(line)` — run a free-form shell line. Stdout/stderr stream
// through `Module.print` (or stdout on the headless platform) exactly
// as before; the return value is the new prompt text, or a V_ERROR on
// dispatch failure. Programmatic callers should prefer typed
// `gs_eval(path, args)` — this method is for the line-input front-end.
static value_t shell_method_run(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("shell.run: expected (line)");

    // dispatch_command mutates the line buffer (tokenizer is in-place),
    // so hand it a writable copy. Strip trailing CR/LF the way em_main.c
    // used to before this method existed.
    char *line = strdup(argv[0].s);
    if (!line)
        return val_err("shell.run: out of memory");
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';

    struct cmd_result res;
    memset(&res, 0, sizeof(res));
    shell_internal_dispatch_command(line, &res);
    free(line);

    char prompt[256];
    shell_build_prompt(prompt, sizeof(prompt));

    if (res.type == RES_ERR) {
        // The dispatcher already printed the error to stderr; return a
        // V_ERROR carrying a brief reason so JS callers can branch on
        // success/failure without parsing the streamed text. The
        // dispatcher's user-facing message lives in `result_buf`.
        return val_err("%s", res.result_buf[0] ? res.result_buf : "command failed");
    }
    return val_str(prompt);
}

// `shell.complete(line, cursor)` — line-level tab completion. Returns
// a V_LIST<V_STRING> of candidates. The `meta.complete` method on the
// synthetic Meta overlay delegates here through the provider hook in
// shell.c, so callers see a single canonical completion engine.
static value_t shell_method_complete(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *line = (argc >= 1 && argv[0].kind == V_STRING && argv[0].s) ? argv[0].s : "";
    int cursor = (int)strlen(line);
    if (argc >= 2) {
        if (argv[1].kind == V_INT)
            cursor = (int)argv[1].i;
        else if (argv[1].kind == V_UINT)
            cursor = (int)argv[1].u;
    }
    struct completion comp;
    memset(&comp, 0, sizeof(comp));
    shell_complete(line, cursor, &comp);
    if (comp.count <= 0)
        return val_list(NULL, 0);
    value_t *items = (value_t *)calloc((size_t)comp.count, sizeof(value_t));
    if (!items)
        return val_err("shell.complete: out of memory");
    for (int i = 0; i < comp.count; i++)
        items[i] = val_str(comp.items[i] ? comp.items[i] : "");
    return val_list(items, (size_t)comp.count);
}

// `shell.expand(text)` — variable / `${...}` / `$(...)` expansion.
// Wraps shell_var_expand for test harnesses and the new UI's terminal
// pre-flight (showing what a typed line will expand to before submit).
static value_t shell_method_expand(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("shell.expand: expected (text)");
    char *out = shell_var_expand(argv[0].s);
    if (!out)
        return val_err("shell.expand: expansion failed");
    value_t v = val_str(out);
    free(out);
    return v;
}

// `shell.script_run(path)` — open a script file and dispatch each
// non-empty, non-comment line through the typed dispatcher. Returns
// the number of lines actually run. Mirrors the headless `script=` flag
// but stays platform-neutral (no scheduler-pumping helper); long-running
// commands inside the script will block the worker until they finish.
static value_t shell_method_script_run(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("shell.script_run: expected (path)");
    const char *path = argv[0].s;
    FILE *f = fopen(path, "r");
    if (!f)
        return val_err("shell.script_run: cannot open '%s'", path);

    int64_t lines_run = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#')
            continue;
        shell_dispatch(line);
        lines_run++;
    }
    fclose(f);
    return val_int(lines_run);
}

// `shell.alias_set(name, expansion)` — thin wrapper over alias_add_user.
static value_t shell_method_alias_set(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    char err[160];
    if (alias_add_user(argv[0].s, argv[1].s, err, sizeof(err)) < 0)
        return val_err("%s", err);
    return val_none();
}

// `shell.alias_unset(name)` — thin wrapper over alias_remove_user.
static value_t shell_method_alias_unset(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    char err[160];
    if (alias_remove_user(argv[0].s, err, sizeof(err)) < 0)
        return val_err("%s", err);
    return val_none();
}

// `shell.interrupt()` — stop the running scheduler. Equivalent to the
// terminal's Ctrl-C path, exposed as a method so JS callers route
// through `gs_eval` like every other interaction.
static value_t shell_method_interrupt(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    scheduler_t *s = system_scheduler();
    if (s)
        scheduler_stop(s);
    return val_none();
}

// === Class descriptor =====================================================

static const arg_decl_t shell_run_args[] = {
    {.name = "line", .kind = V_STRING, .doc = "Free-form shell line"},
};

static const arg_decl_t shell_complete_args[] = {
    {.name = "line", .kind = V_STRING, .doc = "Input line to complete"},
    {.name = "cursor",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Cursor position in line; defaults to end-of-line"},
};

static const arg_decl_t shell_expand_args[] = {
    {.name = "text", .kind = V_STRING, .doc = "Text with ${...} / $(...) references to expand"},
};

static const arg_decl_t shell_script_run_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Script file path"},
};

static const arg_decl_t shell_alias_set_args[] = {
    {.name = "name",      .kind = V_STRING, .doc = "Alias identifier (no $)"          },
    {.name = "expansion", .kind = V_STRING, .doc = "Object path the alias substitutes"},
};

static const arg_decl_t shell_alias_unset_args[] = {
    {.name = "name", .kind = V_STRING, .doc = "Alias identifier (no $)"},
};

static const member_t shell_members[] = {
    {.kind = M_ATTR,
     .name = "prompt",
     .doc = "Current shell prompt text",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = shell_get_prompt, .set = NULL}},
    {.kind = M_ATTR,
     .name = "running",
     .doc = "True while the scheduler is running",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = shell_get_running, .set = NULL}},
    {.kind = M_ATTR,
     .name = "aliases",
     .doc = "List of 'name=path' alias entries (built-in + user)",
     .flags = VAL_RO,
     .attr = {.type = V_LIST, .get = shell_get_aliases, .set = NULL}},
    {.kind = M_ATTR,
     .name = "vars",
     .doc = "List of 'name=value' shell-variable entries",
     .flags = VAL_RO,
     .attr = {.type = V_LIST, .get = shell_get_vars, .set = NULL}},
    {.kind = M_METHOD,
     .name = "run",
     .doc = "Run a free-form shell line; returns the new prompt or V_ERROR",
     .method = {.args = shell_run_args, .nargs = 1, .result = V_STRING, .fn = shell_method_run}},
    {.kind = M_METHOD,
     .name = "complete",
     .doc = "Tab-completion candidates for a partial line",
     .method = {.args = shell_complete_args, .nargs = 2, .result = V_LIST, .fn = shell_method_complete}},
    {.kind = M_METHOD,
     .name = "expand",
     .doc = "Expand ${...} / $(...) references in text",
     .method = {.args = shell_expand_args, .nargs = 1, .result = V_STRING, .fn = shell_method_expand}},
    {.kind = M_METHOD,
     .name = "script_run",
     .doc = "Run every command in a script file; returns the line count",
     .method = {.args = shell_script_run_args, .nargs = 1, .result = V_INT, .fn = shell_method_script_run}},
    {.kind = M_METHOD,
     .name = "alias_set",
     .doc = "Register or replace a user alias",
     .method = {.args = shell_alias_set_args, .nargs = 2, .result = V_NONE, .fn = shell_method_alias_set}},
    {.kind = M_METHOD,
     .name = "alias_unset",
     .doc = "Remove a user alias",
     .method = {.args = shell_alias_unset_args, .nargs = 1, .result = V_NONE, .fn = shell_method_alias_unset}},
    {.kind = M_METHOD,
     .name = "interrupt",
     .doc = "Stop the running scheduler (Ctrl-C path)",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = shell_method_interrupt}},
    // The legacy `shell.alias.{add,remove,list}` sub-namespace stays
    // attached at runtime via root_install (root.c) — the resolver
    // finds it through find_attached_child without a class-level
    // declaration here, so scripts that used the old surface keep
    // working alongside the new `alias_set` / `alias_unset` methods.
};

const class_desc_t shell_class = {
    .name = "Shell",
    .members = shell_members,
    .n_members = sizeof(shell_members) / sizeof(shell_members[0]),
};
