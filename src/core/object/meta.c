// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// meta.c
// The `Meta` class. Every node implicitly carries a `meta` attribute
// whose value is a synthetic Meta node bound to the inspected object.
// See proposal-introspection-via-meta-attribute.md.
//
// Lifetime: meta nodes are allocated lazily on first access and cached
// on the inspected object's private `meta_node` slot. object_delete
// frees the cached meta node before freeing the inspected object, so a
// meta cache cannot outlive its target. Class descriptors themselves
// are immutable for the process lifetime, so the cached node never
// needs invalidation.

#include "meta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object.h"
#include "value.h"

// Provider hook installed by the shell layer. Left NULL in unit tests
// that don't link the shell; `meta.complete(...)` then returns an empty
// list instead of erroring (callers expect a tolerant degradation).
static meta_complete_fn g_complete_provider = NULL;

void meta_set_complete_provider(meta_complete_fn fn) {
    g_complete_provider = fn;
}

// === Path printer ==========================================================
//
// Walks `obj` up to the root. The root carries the substrate name
// ("emu") but doesn't appear in user-facing paths (`cpu.pc`, not
// `emu.cpu.pc`), so the recursion stops as soon as a node has no
// parent. Meta nodes are unattached (parent == NULL), so the recursion
// special-cases them: their path is `<inspected>.meta`.

void object_compute_path(struct object *obj, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0)
        return;
    buf[0] = '\0';
    if (!obj)
        return;

    // Meta node: recurse on the inspected target, then append ".meta".
    if (object_class(obj) == meta_class()) {
        struct object *inspected = (struct object *)object_data(obj);
        object_compute_path(inspected, buf, buf_size);
        size_t len = strlen(buf);
        const char *suffix = (len > 0) ? ".meta" : "meta";
        size_t slen = strlen(suffix);
        if (len + slen + 1 <= buf_size) {
            memcpy(buf + len, suffix, slen + 1);
        }
        return;
    }

    // Root (or detached): empty path.
    struct object *parent = object_parent(obj);
    if (!parent)
        return;

    // Recurse on parent, then append "." + own name.
    object_compute_path(parent, buf, buf_size);
    size_t len = strlen(buf);
    const char *name = object_name(obj);
    if (!name || !*name)
        return;
    size_t nlen = strlen(name);
    // Skip the leading dot when the parent itself was the root (empty).
    bool need_dot = (len > 0);
    if (len + (need_dot ? 1 : 0) + nlen + 1 > buf_size)
        return;
    if (need_dot)
        buf[len++] = '.';
    memcpy(buf + len, name, nlen + 1);
}

// === Member-list accumulator ==============================================
//
// `meta.children`, `meta.attributes`, `meta.methods` all build a
// V_LIST<V_STRING> by scanning the inspected object's class members and
// (for children) its statically-attached children. Same pattern as
// root.c's `objects/attributes/methods` methods.

typedef struct {
    value_t *items;
    size_t len;
    size_t cap;
} name_list_t;

static bool name_list_push(name_list_t *acc, const char *name) {
    if (!name)
        return true;
    if (acc->len + 1 > acc->cap) {
        size_t cap = acc->cap ? acc->cap * 2 : 16;
        value_t *t = (value_t *)realloc(acc->items, cap * sizeof(value_t));
        if (!t)
            return false;
        acc->items = t;
        acc->cap = cap;
    }
    acc->items[acc->len++] = val_str(name);
    return true;
}

static void each_attached_collect(struct object *parent, struct object *child, void *ud) {
    (void)parent;
    name_list_t *acc = (name_list_t *)ud;
    name_list_push(acc, object_name(child));
}

// === Attribute getters ====================================================

// Return the inspected object — i.e. the object the Meta node was
// created for. Stored in instance_data at meta_node_for() time.
static struct object *meta_inspected(struct object *self) {
    return self ? (struct object *)object_data(self) : NULL;
}

static value_t meta_get_class(struct object *self, const member_t *m) {
    (void)m;
    struct object *insp = meta_inspected(self);
    const class_desc_t *cls = insp ? object_class(insp) : NULL;
    return val_str(cls && cls->name ? cls->name : "");
}

static value_t meta_get_doc(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    // Class descriptors do not carry a doc string today. Reserved for a
    // future class_desc_t.doc field; the proposal lists this surface as
    // part of v1 but Record/doc encoding is the §7 open question.
    return val_str("");
}

static value_t meta_get_path(struct object *self, const member_t *m) {
    (void)m;
    struct object *insp = meta_inspected(self);
    char buf[512];
    object_compute_path(insp, buf, sizeof(buf));
    return val_str(buf);
}

static value_t meta_get_children(struct object *self, const member_t *m) {
    (void)m;
    struct object *insp = meta_inspected(self);
    if (!insp)
        return val_list(NULL, 0);
    name_list_t acc = {0};
    const class_desc_t *cls = object_class(insp);
    if (cls) {
        for (size_t i = 0; i < cls->n_members; i++)
            if (cls->members[i].kind == M_CHILD)
                name_list_push(&acc, cls->members[i].name);
    }
    object_each_attached(insp, each_attached_collect, &acc);
    return val_list(acc.items, acc.len);
}

static value_t meta_get_attributes(struct object *self, const member_t *m) {
    (void)m;
    struct object *insp = meta_inspected(self);
    if (!insp)
        return val_list(NULL, 0);
    name_list_t acc = {0};
    const class_desc_t *cls = object_class(insp);
    if (cls) {
        for (size_t i = 0; i < cls->n_members; i++)
            if (cls->members[i].kind == M_ATTR)
                name_list_push(&acc, cls->members[i].name);
    }
    return val_list(acc.items, acc.len);
}

static value_t meta_get_methods(struct object *self, const member_t *m) {
    (void)m;
    struct object *insp = meta_inspected(self);
    if (!insp)
        return val_list(NULL, 0);
    name_list_t acc = {0};
    const class_desc_t *cls = object_class(insp);
    if (cls) {
        for (size_t i = 0; i < cls->n_members; i++)
            if (cls->members[i].kind == M_METHOD)
                name_list_push(&acc, cls->members[i].name);
    }
    return val_list(acc.items, acc.len);
}

// === Methods ==============================================================

// `complete(line, cursor?)` — defer to the shell-installed provider. The
// provider returns a V_LIST<V_STRING> on success or a V_ERROR; when no
// provider is registered (unit tests, headless boot before shell_init),
// an empty list is the tolerant default.
static value_t meta_method_complete(struct object *self, const member_t *m, int argc, const value_t *argv) {
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
    if (!g_complete_provider)
        return val_list(NULL, 0);
    return g_complete_provider(line, cursor);
}

// `member(name)` — short text description of one named member. Cheaper
// than enumerating the full attributes/methods list when the caller
// already knows which member it wants.
static value_t meta_method_member(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("member: expected (name)");
    struct object *insp = meta_inspected(self);
    const class_desc_t *cls = insp ? object_class(insp) : NULL;
    const member_t *mb = class_find_member(cls, argv[0].s);
    if (!mb) {
        // Also probe for a statically-attached child of that name.
        char buf[160];
        snprintf(buf, sizeof(buf), "%s: no member named '%s'", cls && cls->name ? cls->name : "?", argv[0].s);
        return val_err("%s", buf);
    }
    const char *kind_str = "?";
    switch (mb->kind) {
    case M_ATTR:
        kind_str = (mb->flags & VAL_RO) ? "attribute (read-only)" : "attribute";
        break;
    case M_METHOD:
        kind_str = "method";
        break;
    case M_CHILD:
        kind_str = mb->child.indexed ? "child[]" : "child";
        break;
    }
    char buf[320];
    snprintf(buf, sizeof(buf), "%s: %s%s%s", mb->name ? mb->name : "?", kind_str, mb->doc ? " — " : "",
             mb->doc ? mb->doc : "");
    return val_str(buf);
}

// === Class table =========================================================

static const arg_decl_t meta_complete_args[] = {
    {.name = "line", .kind = V_STRING, .doc = "Input line to complete"},
    {.name = "cursor",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Cursor position in line; defaults to end-of-line"},
};

static const arg_decl_t meta_member_args[] = {
    {.name = "name", .kind = V_STRING, .doc = "Member name on the inspected class"},
};

static const member_t meta_members[] = {
    {.kind = M_ATTR,
     .name = "class",
     .doc = "Class name of the inspected node",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = meta_get_class, .set = NULL}},
    {.kind = M_ATTR,
     .name = "doc",
     .doc = "Class doc string (placeholder until class_desc_t.doc lands)",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = meta_get_doc, .set = NULL}},
    {.kind = M_ATTR,
     .name = "path",
     .doc = "Absolute dotted path of the inspected node",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = meta_get_path, .set = NULL}},
    {.kind = M_ATTR,
     .name = "children",
     .doc = "Names of sub-objects on the inspected node",
     .flags = VAL_RO,
     .attr = {.type = V_LIST, .get = meta_get_children, .set = NULL}},
    {.kind = M_ATTR,
     .name = "attributes",
     .doc = "Names of attribute members on the inspected class",
     .flags = VAL_RO,
     .attr = {.type = V_LIST, .get = meta_get_attributes, .set = NULL}},
    {.kind = M_ATTR,
     .name = "methods",
     .doc = "Names of method members on the inspected class",
     .flags = VAL_RO,
     .attr = {.type = V_LIST, .get = meta_get_methods, .set = NULL}},
    {.kind = M_METHOD,
     .name = "complete",
     .doc = "Tab-completion candidates for a partial line",
     .method = {.args = meta_complete_args, .nargs = 2, .result = V_LIST, .fn = meta_method_complete}},
    {.kind = M_METHOD,
     .name = "member",
     .doc = "Short description of one named member",
     .method = {.args = meta_member_args, .nargs = 1, .result = V_STRING, .fn = meta_method_member}},
};

// Special class name — would normally trip the `meta`-is-reserved check
// in object_validate_class, but Meta class members do not collide with
// the reserved literal because none of them are named "meta" themselves.
// Validation is still safe (no member named "meta" appears below).
static const class_desc_t g_meta_class = {
    .name = "Meta",
    .members = meta_members,
    .n_members = sizeof(meta_members) / sizeof(meta_members[0]),
};

const class_desc_t *meta_class(void) {
    return &g_meta_class;
}

// === Cached-node management ==============================================

struct object *meta_node_for(struct object *inspected) {
    if (!inspected)
        return NULL;
    struct object *cached = object_get_meta(inspected);
    if (cached)
        return cached;
    // instance_data carries the back-reference to the inspected node so
    // the Meta getters can read its class / path / member tables.
    struct object *node = object_new(&g_meta_class, inspected, "meta");
    if (!node)
        return NULL;
    object_set_meta(inspected, node);
    return node;
}

void meta_node_release(struct object *inspected) {
    if (!inspected)
        return;
    struct object *cached = object_get_meta(inspected);
    if (!cached)
        return;
    // Clear the slot first so object_delete's transitive meta_node_release
    // call (via the Meta node's own meta cache) sees no cycle.
    object_set_meta(inspected, NULL);
    object_delete(cached);
}
