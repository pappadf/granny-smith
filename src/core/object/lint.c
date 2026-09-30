// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lint.c
// The doc-completeness lint behind shell.lint_members: walks the live tree
// from the root -- every node, attached object, collection entry and
// declared member -- and reports what help, the command browser and the
// argument forms would show empty or wrong.  One line per finding,
// `<path>: <rule>`.
//
// Rules:
//   1. a basic-tier method with an argument whose doc is empty
//   2. an argument of kind V_ANY / V_NONE without OBJ_ARG_POLY
//   3. a method whose result is V_ANY without a result_doc
//   4. a writable V_ENUM attribute, or a V_ENUM argument, without enum_values
//   5. an optional argument whose doc mentions a default but has none
//   6. a node shown in the basic tier whose doc is empty
//   7. an example that does not read as a valid statement against the live
//      tree (checked by the caller's example_ok: a path in it that does not
//      resolve)

#include "lint.h"

#include "meta.h"
#include "object.h"
#include "value.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINT_MAX_VISITED 8192

typedef struct {
    bool (*example_ok)(const char *example);
    value_t *items;
    size_t len, cap;
    struct object *visited[LINT_MAX_VISITED];
    size_t n_visited;
} lint_ctx_t;

// Record one finding, once: the same class instantiated eight times (SCSI
// devices) would otherwise report eight identical member lines per path.
static void report(lint_ctx_t *cx, const char *path, const char *rule) {
    char line[900];
    snprintf(line, sizeof(line), "%s: %s", path, rule);
    for (size_t i = 0; i < cx->len; i++)
        if (strcmp(cx->items[i].s, line) == 0)
            return;
    val_list_push(&cx->items, &cx->len, &cx->cap, val_str(line));
}

static bool seen(lint_ctx_t *cx, struct object *o) {
    for (size_t i = 0; i < cx->n_visited; i++)
        if (cx->visited[i] == o)
            return true;
    if (cx->n_visited < LINT_MAX_VISITED)
        cx->visited[cx->n_visited++] = o;
    return false;
}

// "<parent path>.<name>", or just the name at the root.
static void join(char *out, size_t size, const char *path, const char *name) {
    snprintf(out, size, "%s%s%s", path, *path ? "." : "", name);
}

// Whether `text` mentions "default" in any letter case (strcasestr is a GNU
// extension the wasm libc does not declare).
static bool mentions_default(const char *text) {
    static const char word[] = "default";
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (word[i] && p[i] && tolower((unsigned char)p[i]) == word[i])
            i++;
        if (!word[i])
            return true;
    }
    return false;
}

// The argument rules (1, 2, 4, 5) for one method.
static void lint_args(lint_ctx_t *cx, const char *mpath, const member_t *m) {
    bool basic = (m->flags & M_CAT_MASK) == M_CAT_BASIC && !(m->method.ui_flags & MM_HIDDEN);
    for (int i = 0; i < m->method.nargs && m->method.args; i++) {
        const arg_decl_t *a = &m->method.args[i];
        char p[700];
        snprintf(p, sizeof(p), "%s arg '%s'", mpath, a->name ? a->name : "?");
        if (basic && (!a->doc || !*a->doc))
            report(cx, p, "argument has no doc");
        if ((a->kind == V_ANY || a->kind == V_NONE) && !(a->validation_flags & OBJ_ARG_POLY))
            report(cx, p, "untyped argument (V_ANY/V_NONE) without OBJ_ARG_POLY");
        if (a->kind == V_ENUM && (!a->enum_values || !a->enum_values[0]))
            report(cx, p, "enum argument without enum_values");
        bool has_default = a->default_value && a->default_value->kind != V_NONE;
        if ((a->validation_flags & OBJ_ARG_OPTIONAL) && !has_default && a->doc && mentions_default(a->doc))
            report(cx, p, "doc mentions a default but the argument declares none");
    }
}

static void lint_object(lint_ctx_t *cx, struct object *o, const char *path, bool shown_basic);

// Visit an entry or lookup-backed child object under its path.
static void visit_child(lint_ctx_t *cx, struct object *child, const char *path, bool basic) {
    if (child)
        lint_object(cx, child, path, basic);
}

static void lint_members(lint_ctx_t *cx, struct object *o, const char *path, bool node_basic) {
    const class_desc_t *cls = object_class(o);
    for (size_t i = 0; cls && i < cls->n_members; i++) {
        const member_t *m = &cls->members[i];
        if (!m->name)
            continue;
        char mpath[600];
        join(mpath, sizeof(mpath), path, m->name);
        bool basic = node_basic && (m->flags & M_CAT_MASK) == M_CAT_BASIC;
        switch (m->kind) {
        case M_ATTR:
            if (m->attr.type == V_ENUM && m->attr.set && (!m->attr.enum_values || !m->attr.enum_values[0]))
                report(cx, mpath, "writable enum attribute without enum_values");
            break;
        case M_METHOD: {
            lint_args(cx, mpath, m);
            if (m->method.result == V_ANY && !m->method.result_doc)
                report(cx, mpath, "V_ANY result without result_doc");
            for (size_t e = 0; cx->example_ok && m->examples && m->examples[e]; e++) {
                if (!cx->example_ok(m->examples[e])) {
                    char rule[300];
                    snprintf(rule, sizeof(rule), "example does not resolve: %s", m->examples[e]);
                    report(cx, mpath, rule);
                }
            }
            break;
        }
        case M_CHILD:
            if (m->child.reference)
                break;
            if (m->child.indexed) {
                // Entries of an indexed and/or keyed collection.
                for (int k = object_child_next(o, m, -1); k >= 0; k = object_child_next(o, m, k)) {
                    char ep[640];
                    // `entries` is the container's own member: the entry path
                    // is the container's path with the index.
                    if (strcmp(m->name, "entries") == 0)
                        snprintf(ep, sizeof(ep), "%s[%d]", path, k);
                    else
                        snprintf(ep, sizeof(ep), "%s[%d]", mpath, k);
                    visit_child(cx, m->child.get ? m->child.get(o, k) : NULL, ep, false);
                }
                if (m->child.keys && m->child.lookup) {
                    const char **names = NULL;
                    int n = m->child.keys(o, &names);
                    // Copy: visiting an entry may call keys() again.
                    char **copy = n > 0 ? (char **)calloc((size_t)n, sizeof(char *)) : NULL;
                    for (int k = 0; copy && k < n; k++)
                        copy[k] = strdup(names[k] ? names[k] : "");
                    for (int k = 0; copy && k < n; k++) {
                        char ep[640];
                        if (strcmp(m->name, "entries") == 0)
                            snprintf(ep, sizeof(ep), "%s[\"%s\"]", path, copy[k]);
                        else
                            snprintf(ep, sizeof(ep), "%s[\"%s\"]", mpath, copy[k]);
                        visit_child(cx, m->child.lookup(o, copy[k]), ep, false);
                        free(copy[k]);
                    }
                    free(copy);
                }
            } else {
                struct object *c = m->child.lookup ? m->child.lookup(o, m->name) : NULL;
                if (c) {
                    if (basic && !*object_doc(c) && !(m->doc && *m->doc))
                        report(cx, mpath, "node shown in the basic tier has no doc");
                    visit_child(cx, c, mpath, false);
                }
            }
            break;
        }
    }
}

typedef struct {
    lint_ctx_t *cx;
    const char *path;
    bool basic;
} attached_ctx_t;

static void lint_attached(struct object *parent, struct object *child, void *ud) {
    (void)parent;
    attached_ctx_t *a = (attached_ctx_t *)ud;
    const char *name = object_name(child);
    if (!name)
        return;
    char p[600];
    join(p, sizeof(p), a->path, name);
    bool basic = a->basic && (object_category(child) & M_CAT_MASK) == M_CAT_BASIC;
    if (basic && !*object_doc(child))
        report(a->cx, p, "node shown in the basic tier has no doc");
    lint_object(a->cx, child, p, basic);
}

// `shown_basic`: whether the node itself is reachable in the basic tier
// (its basic attached children are then reported under rule 6).
static void lint_object(lint_ctx_t *cx, struct object *o, const char *path, bool shown_basic) {
    if (seen(cx, o))
        return;
    lint_members(cx, o, path, shown_basic);
    attached_ctx_t a = {.cx = cx, .path = path, .basic = shown_basic};
    object_each_attached_ordered(o, lint_attached, &a);
}

value_t object_lint_members(bool (*example_ok)(const char *example)) {
    lint_ctx_t *cx = (lint_ctx_t *)calloc(1, sizeof(*cx));
    if (!cx)
        return val_err("lint_members: out of memory");
    cx->example_ok = example_ok;
    lint_object(cx, object_root(), "", true);
    value_t out = val_list(cx->items, cx->len);
    free(cx);
    return out;
}
