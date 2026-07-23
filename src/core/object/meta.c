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
    // Attached (runtime) children in deterministic (order, attach_seq)
    // sequence so the SYSTEM tab renders stably (proposal §7.4).
    object_each_attached_ordered(insp, each_attached_collect, &acc);
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

// Map a visibility-category bitfield (M_CAT_*) to its string name. Used by
// the SYSTEM tab / command browser to honour the §7.2 three-tier model.
static const char *category_name(uint16_t flags) {
    switch (flags & M_CAT_MASK) {
    case M_CAT_ADVANCED:
        return "advanced";
    case M_CAT_INTERNAL:
        return "internal";
    default:
        return "basic";
    }
}

// `label` — the inspected node's display label (proposal §7.1). Falls back
// to its path-segment name when no explicit label was set.
static value_t meta_get_label(struct object *self, const member_t *m) {
    (void)m;
    struct object *insp = meta_inspected(self);
    const char *label = object_label(insp);
    return val_str(label ? label : "");
}

// `category` — the inspected node's own visibility tier (basic / advanced /
// internal). Lets the SYSTEM tab decide whether to show a child object
// without a separate allowlist (proposal §7.2 / §8.2).
static value_t meta_get_category(struct object *self, const member_t *m) {
    (void)m;
    struct object *insp = meta_inspected(self);
    return val_str(category_name(object_category(insp)));
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

// `member_category(name)` — visibility tier of a named member on the
// inspected class: "basic" / "advanced" / "internal" (proposal §7.2). The
// SYSTEM tab reads this to decide whether to show an attribute/method row.
// Unknown names default to "basic" (faithful-by-default, §P6).
static value_t meta_method_member_category(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("member_category: expected (name)");
    struct object *insp = meta_inspected(self);
    const member_t *mb = class_find_member(insp ? object_class(insp) : NULL, argv[0].s);
    return val_str(category_name(mb ? mb->flags : 0));
}

// `member_label(name)` — display label of a named member, falling back to the
// name itself (proposal §7.1).
static value_t meta_method_member_label(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("member_label: expected (name)");
    struct object *insp = meta_inspected(self);
    const member_t *mb = class_find_member(insp ? object_class(insp) : NULL, argv[0].s);
    const char *label = (mb && mb->label) ? mb->label : argv[0].s;
    return val_str(label);
}

// `method_info(name)` — UI metadata for a method member, a typed map so the
// context menu and command browser render it without a static catalogue
// (proposal §7.3/§8.3/§8.6): verb label, task category, destructive/mutate/
// hidden flags, declared arg count, and doc. Returns a V_ERROR if the member
// is not a method.
static value_t meta_method_method_info(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("method_info: expected (name)");
    struct object *insp = meta_inspected(self);
    const member_t *mb = class_find_member(insp ? object_class(insp) : NULL, argv[0].s);
    if (!mb || mb->kind != M_METHOD)
        return val_err("method_info: '%s' is not a method", argv[0].s);
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "name", val_str(mb->name ? mb->name : ""));
    val_map_put(b, "verb", val_str(mb->method.verb_label ? mb->method.verb_label : (mb->name ? mb->name : "")));
    val_map_put(b, "category", val_str(category_name(mb->flags)));
    val_map_put(b, "task", val_str(mb->method.task_category ? mb->method.task_category : ""));
    val_map_put(b, "doc", val_str(mb->doc ? mb->doc : ""));
    val_map_put(b, "destructive", val_bool((mb->method.ui_flags & MM_DESTRUCTIVE) != 0));
    val_map_put(b, "mutate", val_bool((mb->method.ui_flags & MM_MUTATE) != 0));
    val_map_put(b, "hidden", val_bool((mb->method.ui_flags & MM_HIDDEN) != 0));
    val_map_put(b, "nargs", val_int((int64_t)mb->method.nargs));
    return val_map_finish(b);
}

// `indices(name)` — the live indices of an indexed-child member (proposal
// §5.3). Lets a tree walker enumerate a sparse collection's occupants
// (machine.scsi.device[0], [3], …) instead of stopping at the bare collection
// member. Returns a V_LIST<V_INT> for an indexed member (possibly empty), or
// a V_ERROR for a non-indexed / unknown member — so a caller can use the
// error/list distinction to tell "indexed collection" from "named child".
static value_t meta_method_indices(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("indices: expected (name)");
    struct object *insp = meta_inspected(self);
    const member_t *mb = class_find_member(insp ? object_class(insp) : NULL, argv[0].s);
    if (!mb || mb->kind != M_CHILD || !mb->child.indexed)
        return val_err("indices: '%s' is not an indexed child", argv[0].s);
    // Walk the member's sparse-index iterator (start at -1). next() returns the
    // next live index or -1 when exhausted; holes are skipped by the callback.
    value_t *items = NULL;
    size_t len = 0, cap = 0;
    if (mb->child.next) {
        for (int i = mb->child.next(insp, -1); i >= 0; i = mb->child.next(insp, i)) {
            if (len + 1 > cap) {
                size_t ncap = cap ? cap * 2 : 8;
                value_t *t = (value_t *)realloc(items, ncap * sizeof(value_t));
                if (!t)
                    break;
                items = t;
                cap = ncap;
            }
            items[len++] = val_int(i);
        }
    }
    return val_list(items, len);
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

static const arg_decl_t meta_named_member_args[] = {
    {.name = "name", .kind = V_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "Member name"},
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
     .name = "label",
     .doc = "Human-facing display label of the inspected node (falls back to name)",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = meta_get_label, .set = NULL}},
    {.kind = M_ATTR,
     .name = "category",
     .doc = "Visibility tier of the inspected node: basic | advanced | internal",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = meta_get_category, .set = NULL}},
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
    {.kind = M_METHOD,
     .name = "member_category",
     .doc = "Visibility tier of a named member: basic | advanced | internal",
     .method = {.args = meta_named_member_args, .nargs = 1, .result = V_STRING, .fn = meta_method_member_category}},
    {.kind = M_METHOD,
     .name = "member_label",
     .doc = "Display label of a named member (falls back to its name)",
     .method = {.args = meta_named_member_args, .nargs = 1, .result = V_STRING, .fn = meta_method_member_label}},
    {.kind = M_METHOD,
     .name = "method_info",
     .doc = "JSON UI metadata for a method (verb, task, destructive, mutate, hidden, nargs)",
     .method = {.args = meta_named_member_args, .nargs = 1, .result = V_MAP, .fn = meta_method_method_info}},
    {.kind = M_METHOD,
     .name = "indices",
     .doc = "Live indices of an indexed-child member (errors if not indexed)",
     .method = {.args = meta_named_member_args, .nargs = 1, .result = V_LIST, .fn = meta_method_indices}},
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
