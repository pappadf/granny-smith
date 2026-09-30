// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// usage.c
// The one renderer of usage text: `help <path>` prints it, `shell.usage`
// returns it together with a method's signature and the byte span of each
// argument in it.  The UI renders what comes back and formats nothing
// itself, so help, the command browser's usage block and the console's
// signature hint cannot drift apart.
//
//   method:    <full.path> <arg> [opt] [rest…] [space: logical | physical]
//              one aligned line per argument: name, type text, doc, default
//              Returns: <result_doc>
//              e.g.  <example>
//              (blank line, then the doc wrapped at 72 columns)
//   attribute: <full.path> : <type text> (read-only)
//               = <value>
//              (blank line, then the doc)
//   node:      <full.path> — <label>, the doc, then attributes / methods /
//              children lists (tiers basic and advanced)

#include "usage.h"

#include "meta.h"
#include "object.h"
#include "value.h"
#include "value_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USAGE_WRAP 72

// Append a NUL-terminated string.
static void put(vbuf_t *b, const char *s) {
    vbuf_append(b, s, strlen(s));
}

// Drop trailing spaces from the buffer (before a newline is added).
static void trim_trailing(vbuf_t *b) {
    while (b->len > 0 && b->p[b->len - 1] == ' ')
        b->p[--b->len] = '\0';
}

// End the current line: trailing whitespace removed, then '\n'.
static void newline(vbuf_t *b) {
    trim_trailing(b);
    put(b, "\n");
}

// Append `n` spaces.
static void pad(vbuf_t *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        put(b, " ");
}

// Display width of a UTF-8 string: its code points (continuation bytes do
// not count), which is what padding needs for "…" and "—".
static size_t text_width(const char *s) {
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80)
            w++;
    return w;
}

// Greedy word wrap of `text` at USAGE_WRAP columns; each output line gets
// `indent` spaces first (the first line too when `indent_first`).  A word
// longer than the width stays whole.
static void wrap(vbuf_t *b, const char *text, size_t indent, bool indent_first) {
    size_t col = 0;
    bool line_empty = true;
    if (indent_first) {
        pad(b, indent);
        col = indent;
    }
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *e = p;
        while (*e && *e != ' ' && *e != '\n' && *e != '\t')
            e++;
        size_t wlen = (size_t)(e - p);
        char word[512];
        size_t cpy = wlen < sizeof(word) - 1 ? wlen : sizeof(word) - 1;
        memcpy(word, p, cpy);
        word[cpy] = '\0';
        size_t w = text_width(word);
        if (!line_empty && col + 1 + w > USAGE_WRAP) {
            newline(b);
            pad(b, indent);
            col = indent;
            line_empty = true;
        }
        if (!line_empty) {
            put(b, " ");
            col++;
        }
        vbuf_append(b, p, wlen);
        col += w;
        line_empty = false;
        p = e;
    }
}

// REPL text of a value: its scalar text, or compact JSON for a list or map.
static void value_text(vbuf_t *b, const value_t *v) {
    if (v->kind == V_LIST || v->kind == V_MAP)
        value_format(v, VFMT_JSON, b);
    else
        value_format(v, VFMT_REPL, b);
}

// Type text: the kind, plus ", hex" / ", bin" / ", path" for those
// presentations; enums say "enum".
static void type_text(vbuf_t *b, value_kind_t kind, uint16_t presentation) {
    put(b, value_kind_name(kind));
    const char *pres = meta_presentation_text(presentation);
    if (pres && (strcmp(pres, "hex") == 0 || strcmp(pres, "bin") == 0 || strcmp(pres, "path") == 0)) {
        put(b, ", ");
        put(b, pres);
    }
}

// An argument's kind for its type text: a V_NONE argument accepts any value
// (the declaration's older spelling of V_ANY), so it reads "any".
static value_kind_t arg_kind(const arg_decl_t *a) {
    return a->kind == V_NONE ? V_ANY : a->kind;
}

// The path of `obj` plus `.member` (no leading dot at the root).
static void full_path(vbuf_t *b, struct object *obj, const char *member) {
    char buf[512];
    object_compute_path(obj, buf, sizeof(buf));
    put(b, buf);
    if (member) {
        if (buf[0])
            put(b, ".");
        put(b, member);
    }
}

// The object `path` addresses: resolving `<path>.meta` answers the Meta
// node bound to it, whichever way the path got there (attached child,
// lookup-backed child, collection entry).
static struct object *path_object(const char *path) {
    char buf[600];
    snprintf(buf, sizeof(buf), "%s%smeta", path, *path ? "." : "");
    node_t m = object_resolve(object_root(), buf);
    if (!node_valid(m) || m.member)
        return NULL;
    return (struct object *)object_data(m.obj);
}

// Signature of a method, with the byte span of each argument's bracketed
// form.  Writes the signature into `b` and the spans into `spans`.
static value_t method_signature(vbuf_t *b, struct object *obj, const member_t *m) {
    full_path(b, obj, m->name);
    value_t *spans = NULL;
    size_t len = 0, cap = 0;
    for (int i = 0; i < m->method.nargs && m->method.args; i++) {
        const arg_decl_t *a = &m->method.args[i];
        bool opt = (a->validation_flags & OBJ_ARG_OPTIONAL) != 0;
        bool rest = (a->validation_flags & OBJ_ARG_REST) != 0;
        put(b, " ");
        size_t start = b->len;
        put(b, (opt || rest) ? "[" : "<");
        put(b, a->name ? a->name : "?");
        if (rest)
            put(b, "…");
        if (a->kind == V_ENUM && a->enum_values && a->enum_values[0]) {
            put(b, ": ");
            for (size_t k = 0; a->enum_values[k]; k++) {
                if (k)
                    put(b, " | ");
                put(b, a->enum_values[k]);
            }
        }
        put(b, (opt || rest) ? "]" : ">");
        value_t pair[2] = {val_uint(4, start), val_uint(4, b->len)};
        value_t *pv = (value_t *)malloc(sizeof(pair));
        if (pv)
            memcpy(pv, pair, sizeof(pair));
        val_list_push(&spans, &len, &cap, pv ? val_list(pv, 2) : val_none());
    }
    return val_list(spans, len);
}

// Usage text for a method (text starts with the signature).
static void method_text(vbuf_t *t, const char *sig, const member_t *m) {
    put(t, sig);
    newline(t);
    size_t name_w = 0, type_w = 0;
    for (int i = 0; i < m->method.nargs && m->method.args; i++) {
        const arg_decl_t *a = &m->method.args[i];
        size_t nw = text_width(a->name ? a->name : "");
        if (nw > name_w)
            name_w = nw;
        vbuf_t tt = {0};
        type_text(&tt, arg_kind(a), a->presentation_flags);
        if (tt.p && strlen(tt.p) > type_w)
            type_w = strlen(tt.p);
        vbuf_free(&tt);
    }
    for (int i = 0; i < m->method.nargs && m->method.args; i++) {
        const arg_decl_t *a = &m->method.args[i];
        put(t, "  ");
        put(t, a->name ? a->name : "");
        pad(t, name_w + 2 - text_width(a->name ? a->name : ""));
        vbuf_t tt = {0};
        type_text(&tt, arg_kind(a), a->presentation_flags);
        put(t, tt.p ? tt.p : "");
        pad(t, type_w + 2 - (tt.p ? strlen(tt.p) : 0));
        vbuf_free(&tt);
        char doc[512];
        arg_doc_text(a, doc, sizeof(doc));
        put(t, doc);
        if (arg_has_default(a)) {
            put(t, " (default ");
            value_text(t, a->default_value);
            put(t, ")");
        }
        newline(t);
    }
    if (m->method.result_doc) {
        put(t, "Returns: ");
        put(t, m->method.result_doc);
        newline(t);
    }
    for (size_t i = 0; m->examples && m->examples[i]; i++) {
        put(t, i == 0 ? "e.g.  " : "      ");
        put(t, m->examples[i]);
        newline(t);
    }
    if (m->doc && *m->doc) {
        newline(t);
        wrap(t, m->doc, 0, false);
        newline(t);
    }
}

// Usage text for an attribute.
static void attr_text(vbuf_t *t, struct object *obj, const member_t *m) {
    full_path(t, obj, m->name);
    put(t, " : ");
    if (m->attr.type == V_ENUM)
        put(t, "enum");
    else
        type_text(t, m->attr.type, m->attr.presentation_flags);
    if (member_is_readonly(m))
        put(t, " (read-only)");
    newline(t);
    if (!(m->attr.presentation_flags & VAL_SENSITIVE)) {
        value_t v = node_get((node_t){.obj = obj, .member = m, .index = -1});
        if (v.kind != V_ERROR) {
            put(t, " = ");
            value_text(t, &v);
            newline(t);
        }
        value_free(&v);
    }
    if (m->doc && *m->doc) {
        newline(t);
        wrap(t, m->doc, 0, false);
        newline(t);
    }
}

// Name list accumulated for a node's summary lines.
typedef struct {
    vbuf_t names;
    bool any;
} name_acc_t;

static void acc_add(name_acc_t *a, const char *name) {
    if (a->any)
        put(&a->names, ", ");
    put(&a->names, name);
    a->any = true;
}

static void acc_attached(struct object *parent, struct object *child, void *ud) {
    (void)parent;
    if ((object_category(child) & M_CAT_MASK) == M_CAT_INTERNAL || !object_name(child))
        return;
    acc_add((name_acc_t *)ud, object_name(child));
}

// One `label: a, b, …` line, wrapped with a two-space hanging indent.
static void list_line(vbuf_t *t, const char *label, name_acc_t *a) {
    if (!a->any)
        return;
    vbuf_t line = {0};
    put(&line, label);
    put(&line, ": ");
    put(&line, a->names.p);
    wrap(t, line.p, 2, false);
    newline(t);
    vbuf_free(&line);
}

// Usage text for a node (an object).
static void node_text(vbuf_t *t, struct object *obj) {
    vbuf_t path = {0};
    full_path(&path, obj, NULL);
    put(t, path.p && *path.p ? path.p : "(root)");
    vbuf_free(&path);
    put(t, " — ");
    put(t, object_label(obj) ? object_label(obj) : "");
    newline(t);
    const char *doc = object_doc(obj);
    if (doc && *doc) {
        wrap(t, doc, 0, false);
        newline(t);
    }
    name_acc_t attrs = {0}, methods = {0}, children = {0};
    const class_desc_t *cls = object_class(obj);
    for (size_t i = 0; cls && i < cls->n_members; i++) {
        const member_t *m = &cls->members[i];
        if (!member_is_listed(m))
            continue;
        if (m->kind == M_ATTR)
            acc_add(&attrs, m->name);
        else if (m->kind == M_METHOD)
            acc_add(&methods, m->name);
        else if (m->kind == M_CHILD)
            acc_add(&children, m->name);
    }
    object_each_attached_ordered(obj, acc_attached, &children);
    if (attrs.any || methods.any || children.any) {
        newline(t);
        list_line(t, "attributes", &attrs);
        list_line(t, "methods", &methods);
        list_line(t, "children", &children);
    }
    vbuf_free(&attrs.names);
    vbuf_free(&methods.names);
    vbuf_free(&children.names);
}

// Remove the final newline (text has no trailing whitespace).
static void finish(vbuf_t *t) {
    while (t->len > 0 && (t->p[t->len - 1] == '\n' || t->p[t->len - 1] == ' '))
        t->p[--t->len] = '\0';
}

static usage_word_t (*g_word_resolver)(const char *word, char *target, size_t target_size);

void object_usage_set_word_resolver(usage_word_t (*resolve)(const char *word, char *target, size_t target_size)) {
    g_word_resolver = resolve;
}

static value_t usage_map(const char *sig, value_t spans, const char *text) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "signature", val_str(sig));
    val_map_put(b, "arg_spans", spans);
    val_map_put(b, "text", val_str(text));
    return val_map_finish(b);
}

value_t object_usage(const char *path) {
    node_t n = object_resolve(object_root(), path ? path : "");
    // A word that is no path: a function (a note), or a command (the usage
    // of the method it runs, with a closing note).
    char target[256] = "";
    const char *word = path ? path : "";
    if (!node_valid(n) && g_word_resolver) {
        usage_word_t w = g_word_resolver(word, target, sizeof(target));
        if (w == USAGE_WORD_FUNCTION) {
            vbuf_t t = {0};
            put(&t, "`");
            put(&t, word);
            put(&t, "` is a function, defined with def.");
            value_t r = usage_map("", val_list(NULL, 0), t.p);
            vbuf_free(&t);
            return r;
        }
        if (w == USAGE_WORD_COMMAND)
            n = object_resolve(object_root(), target);
        else
            target[0] = '\0';
    }
    if (!node_valid(n))
        return val_err("usage: path '%s' did not resolve", path ? path : "");
    vbuf_t sig = {0}, text = {0};
    value_t spans = val_list(NULL, 0);
    if (n.member && n.member->kind == M_METHOD) {
        value_free(&spans);
        spans = method_signature(&sig, n.obj, n.member);
        method_text(&text, sig.p ? sig.p : "", n.member);
    } else if (n.member && n.member->kind == M_ATTR) {
        attr_text(&text, n.obj, n.member);
    } else {
        struct object *obj = path_object(path ? path : "");
        if (!obj) {
            value_free(&spans);
            return val_err("usage: '%s' names no object right now", path ? path : "");
        }
        node_text(&text, obj);
    }
    if (target[0]) {
        finish(&text);
        put(&text, "\n\n`");
        put(&text, word);
        put(&text, "` is a command: it runs ");
        put(&text, target);
        put(&text, ".");
    }
    finish(&text);
    value_t r = usage_map(sig.p ? sig.p : "", spans, text.p ? text.p : "");
    vbuf_free(&sig);
    vbuf_free(&text);
    return r;
}

value_t object_usage_text(const char *path) {
    value_t u = object_usage(path);
    if (u.kind != V_MAP)
        return u;
    value_t out = val_str("");
    for (size_t i = 0; i < u.map.len; i++)
        if (strcmp(u.map.entries[i].key, "text") == 0) {
            value_free(&out);
            out = value_dup(&u.map.entries[i].val);
        }
    value_free(&u);
    return out;
}
