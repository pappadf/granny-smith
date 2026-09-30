// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell_class.c
// The `Shell` class on the object root — the last subsystem to enter
// the object model. After this lands, the JS bridge's free-form-line
// kind (pending=4) retires; every JS→C call rides on `gs_eval` (kind=1),
// either against typed paths (`cpu.pc`) or against the shell's own
// methods (`shell.run`, `shell.complete`, `shell.expand`, …).
//
// The Shell class is a thin wrapper. Its method bodies forward to the
// existing shell internals (the static `dispatch_command` in shell.c
// reached via shell_internal.h, `shell_complete`, `shell_var_expand`,
// the alias-table API). No business logic moves here.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmd_complete.h"
#include "expr.h"
#include "highlight.h"
#include "lint.h"
#include "object.h"
#include "scheduler.h"
#include "script.h"
#include "shell.h"
#include "usage.h"
#include "event/gs_event.h"
#include "job/job.h"

#include "shell_internal.h"
#include "shell_var.h"
#include "system.h"
#include "value.h"
#include "value_format.h"

// === Attribute getters ====================================================

// `shell.prompt` — current prompt text. Read once per terminal redraw.
static value_t shell_get_prompt(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    char buf[256];
    shell_build_prompt(buf, sizeof(buf));
    return val_str(buf);
}

// `shell.running` — true while the scheduler is running. The intent is
// "true while a command is in flight"; in practice the
// only commands that meaningfully run are scheduler-driven (the rest
// finish synchronously), so this is the right proxy.
static value_t shell_get_running(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    scheduler_t *s = system_scheduler();
    return val_bool(s ? scheduler_is_running(s) : false);
}

// Growable list of V_STRING items, for `shell.vars`.
typedef struct {
    value_t *items;
    size_t len;
    size_t cap;
} str_list_t;

// The shared accumulator; this was the fourth of five copies.
static bool str_list_push(str_list_t *acc, const char *s) {
    if (!s)
        return true;
    if (!val_list_push(&acc->items, &acc->len, &acc->cap, val_str(s)))
        return false;
    return true;
}

// `shell.vars` — list of "name=value" strings. Iteration walks the
// internal table; for V_STRING entries the value is rendered verbatim,
// other kinds emit their JSON-ish formatter shape.
// `shell.vars` renders each entry as `name=value`.  This was an EIGHTH
// per-kind formatter, with its own cruder default (`<%d>` for every kind it
// did not name, including objects and errors) and its own habit of ignoring
// VAL_HEX.  It is a table
// cell by any other name, so it is one now.
static void format_value_compact(const value_t *v, char *buf, size_t buf_size) {
    value_format_into(v, VFMT_CELL, buf, buf_size);
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

    // The interpreter mutates its line buffer, so hand it a writable
    // copy. Strip trailing CR/LF the way em_main.c used to before this
    // method existed.
    char *line = strdup(argv[0].s);
    if (!line)
        return val_err("shell.run: out of memory");
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';

    char err[128] = {0};
    bool ok = shell_internal_dispatch_command(line, err, sizeof(err));
    free(line);

    // The dispatcher already printed any error to stderr; on failure
    // return a V_ERROR carrying a brief reason so JS callers can branch
    // on success without parsing the streamed text. On success return
    // the new prompt text.
    if (!ok)
        return val_err("%s", err[0] ? err : "command failed");

    char prompt[256];
    shell_build_prompt(prompt, sizeof(prompt));
    return val_str(prompt);
}

// `shell.complete(line, cursor)` — line-level tab completion. Returns
// {candidates: V_LIST<V_STRING>, span: {start, end}} where span is the
// half-open range of line text each candidate replaces (object-path
// candidates cover the whole word; filesystem candidates only the
// basename).  With detail, candidates are {text, kind, doc} and the map
// adds `context` and `truncated`. The `meta.complete` method on the synthetic Meta overlay
// delegates here through the provider hook in shell.c and keeps the
// bare-list shape.
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
    bool detail = argc >= 3 && argv[2].kind == V_BOOL && argv[2].b;
    struct completion comp;
    memset(&comp, 0, sizeof(comp));
    shell_complete(line, cursor, &comp);
    value_t *items = NULL;
    if (comp.count > 0) {
        items = (value_t *)calloc((size_t)comp.count, sizeof(value_t));
        if (!items)
            return val_err("shell.complete: out of memory");
        for (int i = 0; i < comp.count; i++) {
            const char *text = comp.items[i] ? comp.items[i] : "";
            if (!detail) {
                items[i] = val_str(text);
                continue;
            }
            // Detail: {text, kind, doc} per candidate.
            value_map_builder_t *c = val_map_new();
            val_map_put(c, "text", val_str(text));
            val_map_put(c, "kind", val_str(comp_kind_name((comp_kind_t)comp.kinds[i])));
            val_map_put(c, "doc", val_str(comp.docs[i] ? comp.docs[i] : ""));
            items[i] = val_map_finish(c);
        }
    }
    value_map_builder_t *span = val_map_new();
    val_map_put(span, "start", val_int(comp.start));
    val_map_put(span, "end", val_int(comp.end));
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "candidates", val_list(items, comp.count > 0 ? (size_t)comp.count : 0));
    val_map_put(b, "span", val_map_finish(span));
    if (detail) {
        // Where the cursor sits: the method and the declared argument slot
        // it fills, all none outside an argument position.
        value_map_builder_t *ctx = val_map_new();
        val_map_put(ctx, "method", comp.has_context ? val_str(comp.ctx_method) : val_none());
        val_map_put(ctx, "arg_index",
                    comp.has_context && comp.ctx_arg_index >= 0 ? val_int(comp.ctx_arg_index) : val_none());
        val_map_put(ctx, "arg_name", comp.ctx_arg_name ? val_str(comp.ctx_arg_name) : val_none());
        val_map_put(b, "context", val_map_finish(ctx));
        // Whether candidates were dropped (the item table or the pool filled).
        val_map_put(b, "truncated", val_bool(comp.truncated));
    }
    return val_map_finish(b);
}

// `shell.expand(text)` — interpolate a string body (`${…}` / `$name`)
// against the current bindings. Retained for test harnesses; line-level
// preprocessing no longer exists in v2, so this is exactly the
// dq-string interpolator.
static value_t shell_method_expand(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("shell.expand: expected (text)");
    expr_ctx_t ectx;
    script_expr_ctx(&ectx);
    return expr_interpolate_body(argv[0].s, &ectx);
}

// `shell.script_run(path)` — parse and execute the file with the v2
// script interpreter via script_run_file, so `include` paths inside it
// resolve relative to the file. Returns V_NONE on success, V_ERROR if
// the script aborted.
static value_t shell_method_script_run(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("shell.script_run: expected (path)");
    const char *path = argv[0].s;
    if (script_run_file(path) != 0)
        return val_err("shell.script_run: '%s' failed", path);
    return val_none();
}

// `shell.eval(text)` — run a (possibly multi-line) script source string
// through the interpreter. The JS terminal and tests use this to submit
// brace-balanced buffers without touching disk.
static value_t shell_method_eval(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    if (argv[0].kind != V_STRING || !argv[0].s)
        return val_err("shell.eval: expected (text)");
    if (script_run_source(argv[0].s) != 0)
        return val_err("shell.eval: script failed");
    return val_none();
}

// `shell.interrupt()` — stop the running scheduler and cancel any
// running script loop at its next iteration check. Equivalent
// to the terminal's Ctrl-C path, exposed as a method so JS callers
// route through `gs_eval` like every other interaction.
static value_t shell_method_interrupt(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    // "Cancel my job, or stop my mode": the client being served owns what
    // it interrupts and nothing else.  Outside a request (client 0: the
    // headless REPL's own line) it is the old unconditional stop.
    uint32_t client = gs_current_client();
    if (client == 0) {
        scheduler_t *s = system_scheduler();
        if (s)
            scheduler_stop(s);
        script_interrupt();
        return val_none();
    }
    // From inside a script it is the script asking: stop my run, not me.
    if (!job_serving_call())
        job_cancel_client(client);
    job_glue_stop_modes(client);
    return val_none();
}

// `shell.usage(path)` — {signature, arg_spans, text} (usage.c); `help`
// prints the text.
static value_t shell_method_usage(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    return object_usage(argv[0].s);
}

// `shell.highlight(text)` — syntax classes for a line or block (highlight.c):
// what the console colours as the user types.
static value_t shell_method_highlight(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    return shell_highlight(argv[0].s ? argv[0].s : "");
}

// `shell.needs_continuation(text)` — true while `text` is an incomplete
// statement or block, so a console knows whether Enter submits or breaks
// the line.
static value_t shell_method_needs_continuation(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    return val_bool(script_needs_continuation(argv[0].s ? argv[0].s : ""));
}

// A method example passes when shell.highlight finds nothing unresolved in it.
static bool example_resolves(const char *example) {
    value_t spans = shell_highlight(example);
    bool ok = spans.kind == V_LIST;
    for (size_t i = 0; ok && i < spans.list.len; i++) {
        const value_t *cls = value_map_get(&spans.list.items[i], "class");
        if (cls && cls->kind == V_STRING && cls->s && strcmp(cls->s, "unknown") == 0)
            ok = false;
    }
    value_free(&spans);
    return ok;
}

// `shell.lint_members()` — the doc-completeness lint (lint.c), with the
// examples checked against the live tree.
static value_t shell_method_lint_members(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    return object_lint_members(example_resolves);
}

// `shell.keywords` — [{word, syntax}] for every keyword (object.c's table).
static value_t shell_get_keywords(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    value_t *items = NULL;
    size_t len = 0, cap = 0;
    for (size_t i = 0; i < object_keyword_count(); i++) {
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "word", val_str(object_keyword(i)));
        val_map_put(b, "syntax", val_str(object_keyword_syntax(i)));
        val_list_push(&items, &len, &cap, val_map_finish(b));
    }
    return val_list(items, len);
}

// === Class descriptor =====================================================

static const arg_decl_t shell_usage_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Path of a method, attribute or node"},
};

static const arg_decl_t shell_text_args[] = {
    {.name = "text", .kind = V_STRING, .doc = "Statement or block text"},
};

static const arg_decl_t shell_run_args[] = {
    {.name = "line", .kind = V_STRING, .doc = "Free-form shell line"},
};

static const value_t shell_false = {.kind = V_BOOL, .b = false};

static const arg_decl_t shell_complete_args[] = {
    {.name = "line", .kind = V_STRING, .doc = "Input line to complete"},
    {.name = "cursor",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &obj_arg_unset,
     .doc = "Cursor position in line (a byte offset); omitted: the end of the line"},
    {.name = "detail",
     .kind = V_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &shell_false,
     .doc = "Return {text, kind, doc} candidates and the argument context"},
};

static const arg_decl_t shell_expand_args[] = {
    {.name = "text", .kind = V_STRING, .doc = "Text with ${...} / $(...) references to expand"},
};

static const arg_decl_t shell_script_run_args[] = {
    {.name = "path", .kind = V_STRING, .presentation_flags = VAL_PATH, .doc = "Script file path"},
};

static const arg_decl_t shell_eval_args[] = {
    {.name = "text", .kind = V_STRING, .doc = "Script source (may span multiple lines)"},
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
     .name = "vars",
     .doc = "List of 'name=value' shell-variable entries",
     .flags = VAL_RO,
     .attr = {.type = V_LIST, .get = shell_get_vars, .set = NULL}},
    {.kind = M_ATTR,
     .name = "keywords",
     .doc = "Every keyword with its one-line syntax: [{word, syntax}]",
     .flags = VAL_RO,
     .attr = {.type = V_LIST, .get = shell_get_keywords, .set = NULL}},
    {.kind = M_METHOD,
     .name = "lint_members",
     .flags = M_CAT_INTERNAL,
     .doc = "Documentation gaps in the live tree: '<path>: <rule>' lines",
     .method = {.args = NULL, .nargs = 0, .result = V_LIST, .fn = shell_method_lint_members}},
    {.kind = M_METHOD,
     .name = "usage",
     .doc = "Usage of a path as {signature, arg_spans, text}; help prints the text",
     .method = {.args = shell_usage_args, .nargs = 1, .result = V_MAP, .fn = shell_method_usage}},
    {.kind = M_METHOD,
     .name = "needs_continuation",
     .doc = "True while text is an incomplete statement or block",
     .method = {.ui_flags = MM_HIDDEN,
                .args = shell_text_args,
                .nargs = 1,
                .result = V_BOOL,
                .fn = shell_method_needs_continuation}},
    {.kind = M_METHOD,
     .name = "highlight",
     .doc = "Syntax classes of a line or block: a list of {start, end, class} spans (UTF-8 byte offsets)",
     .method =
         {.ui_flags = MM_HIDDEN, .args = shell_text_args, .nargs = 1, .result = V_LIST, .fn = shell_method_highlight}},
    {.kind = M_METHOD,
     .name = "run",
     .doc = "Run a free-form shell line; returns the new prompt or V_ERROR",
     .method = {.ui_flags = MM_HIDDEN, .args = shell_run_args, .nargs = 1, .result = V_STRING, .fn = shell_method_run}},
    {.kind = M_METHOD,
     .name = "complete",
     .doc = "Tab completion: {candidates, span:{start,end}} for a partial line; with detail, candidates are "
            "{text, kind, doc} and a context says which method argument the cursor is in", .method = {.ui_flags = MM_HIDDEN,
                .args = shell_complete_args,
                .nargs = 3,
                .result = V_MAP,
                .fn = shell_method_complete}},
    {.kind = M_METHOD,
     .name = "expand",
     .doc = "Expand ${...} / $(...) references in text",
     .method = {.args = shell_expand_args, .nargs = 1, .result = V_STRING, .fn = shell_method_expand}},
    {.kind = M_METHOD,
     .name = "script_run",
     .doc = "Parse and run a script file (block-aware); errors abort the script",
     .method = {.args = shell_script_run_args, .nargs = 1, .result = V_NONE, .fn = shell_method_script_run}},
    {.kind = M_METHOD,
     .name = "eval",
     .doc = "Run a (possibly multi-line) script source string",
     .method = {.args = shell_eval_args, .nargs = 1, .result = V_NONE, .fn = shell_method_eval}},
    {.kind = M_METHOD,
     .name = "interrupt",
     .doc = "Stop the running scheduler (Ctrl-C path)",
     .method = {.ui_flags = MM_HIDDEN, .args = NULL, .nargs = 0, .result = V_NONE, .fn = shell_method_interrupt}},
    // `shell.alias` and `shell.command` are attached at runtime by
    // root_install (root.c); the resolver finds them through
    // find_attached_child, without a declaration here.
};

const class_desc_t shell_class = {
    .name = "Shell",
    .members = shell_members,
    .n_members = sizeof(shell_members) / sizeof(shell_members[0]),
    .doc = "The shell: bindings, functions, aliases and scripts",
};
