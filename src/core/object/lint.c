// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lint.c
// The doc-completeness lint behind shell.lint_members: walks the live tree
// (object_walk) and reports what help, the command browser and the argument
// forms would show empty or wrong.  One line per finding, `<path>: <rule>`.
//
// Per class, once, under the path of the first object of that class:
//   - the members' own documentation gaps (object_member_doc_gaps: argument
//     docs, untyped arguments, V_ANY results)
//   - an example that does not read as a valid statement against the live
//     tree (checked by the caller's example_ok: a path in it that does not
//     resolve)
// Per node:
//   - a node shown in the basic tier whose doc is empty
//
// What a class declaration gets wrong outright (an enum argument without its
// values, and the like) is object_validate_class's to refuse.

#include "lint.h"

#include "object.h"
#include "value.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    bool (*example_ok)(const char *example);
    value_t *items;
    size_t len, cap;
    const char *path; // the member path the class gaps are reported under
} lint_ctx_t;

// Record one finding.
static void report(lint_ctx_t *cx, const char *path, const char *rule) {
    char line[900];
    snprintf(line, sizeof(line), "%s: %s", path, rule);
    val_list_push(&cx->items, &cx->len, &cx->cap, val_str(line));
}

// A node shown in the basic tier needs a doc: its own, or its member's.
static bool lint_object(struct object *o, const member_t *via, const char *path, bool basic, bool first, void *ud) {
    (void)first;
    if (basic && *path && !*object_doc(o) && !(via && via->doc && *via->doc))
        report((lint_ctx_t *)ud, path, "node shown in the basic tier has no doc");
    return true;
}

// One class documentation gap, under the member's path.
static void lint_gap(const member_t *m, const arg_decl_t *a, const char *rule, void *ud) {
    (void)m;
    lint_ctx_t *cx = (lint_ctx_t *)ud;
    if (!a) {
        report(cx, cx->path, rule);
        return;
    }
    char p[700];
    snprintf(p, sizeof(p), "%s arg '%s'", cx->path, a->name ? a->name : "?");
    report(cx, p, rule);
}

// The class rules, checked on the first object of each class.
static void lint_member(struct object *o, const member_t *m, const char *path, bool first, void *ud) {
    lint_ctx_t *cx = (lint_ctx_t *)ud;
    if (!first || m->kind != M_METHOD)
        return;
    (void)o;
    cx->path = path;
    object_member_doc_gaps(m, lint_gap, cx);
    for (size_t e = 0; cx->example_ok && m->examples && m->examples[e]; e++) {
        if (!cx->example_ok(m->examples[e])) {
            char rule[300];
            snprintf(rule, sizeof(rule), "example does not resolve: %s", m->examples[e]);
            report(cx, path, rule);
        }
    }
}

value_t object_lint_members(bool (*example_ok)(const char *example)) {
    lint_ctx_t cx = {.example_ok = example_ok};
    const object_visitor_t v = {.object = lint_object, .member = lint_member};
    object_walk(object_root(), &v, &cx);
    return val_list(cx.items, cx.len);
}
