// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// meta.c
// The `Meta` class. Every node implicitly carries a `meta` attribute
// whose value is a synthetic Meta node bound to the inspected object.
// See docs/internals/core/object/object-model.md.
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

    // Callback-backed child (collection entry, lookup-backed named child):
    // no attached parent, but a logical one -- `<parent>.<name>`,
    // `<parent>[<index>]` or `<parent>["<key>"]`.
    struct object *parent = object_parent(obj);
    struct object *lparent = parent ? NULL : object_logical_parent(obj);
    if (lparent) {
        object_compute_path(lparent, buf, buf_size);
        size_t len = strlen(buf);
        char seg[OBJ_KEY_MAX + 8];
        const char *lname = object_logical_name(obj);
        const char *lkey = object_logical_key(obj);
        if (lname)
            snprintf(seg, sizeof(seg), "%s%s", len > 0 ? "." : "", lname);
        else if (lkey)
            snprintf(seg, sizeof(seg), "[\"%s\"]", lkey);
        else
            snprintf(seg, sizeof(seg), "[%d]", object_logical_index(obj));
        size_t slen = strlen(seg);
        if (len + slen + 1 <= buf_size)
            memcpy(buf + len, seg, slen + 1);
        return;
    }

    // Root (or detached): empty path.
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
    bool oom; // set when a push failed; the list must not be returned short
} name_list_t;

// The shared accumulator, val_list_push.  This was the second of five
// near-identical copies, and like the others it discarded the failure -- so
// meta.children and meta.indices returned a SILENTLY TRUNCATED list under
// memory pressure, which the inspector renders as the complete set.
static bool name_list_push(name_list_t *acc, const char *name) {
    if (!name)
        return true;
    return val_list_push(&acc->items, &acc->len, &acc->cap, val_str(name));
}

static void each_attached_collect(struct object *parent, struct object *child, void *ud) {
    (void)parent;
    name_list_t *acc = (name_list_t *)ud;
    if (!name_list_push(acc, object_name(child)))
        acc->oom = true;
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
    (void)m;
    return val_str(object_doc(meta_inspected(self)));
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
                if (!name_list_push(&acc, cls->members[i].name))
                    acc.oom = true;
    }
    // Attached (runtime) children in deterministic (order, attach_seq)
    // sequence so the SYSTEM tab renders stably.
    object_each_attached_ordered(insp, each_attached_collect, &acc);
    if (acc.oom) {
        for (size_t i = 0; i < acc.len; i++)
            value_free(&acc.items[i]);
        free(acc.items);
        return val_err("out of memory");
    }
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
                if (!name_list_push(&acc, cls->members[i].name))
                    acc.oom = true;
    }
    if (acc.oom) {
        for (size_t i = 0; i < acc.len; i++)
            value_free(&acc.items[i]);
        free(acc.items);
        return val_err("out of memory");
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
                if (!name_list_push(&acc, cls->members[i].name))
                    acc.oom = true;
    }
    if (acc.oom) {
        for (size_t i = 0; i < acc.len; i++)
            value_free(&acc.items[i]);
        free(acc.items);
        return val_err("out of memory");
    }
    return val_list(acc.items, acc.len);
}

// Map a visibility-category bitfield (M_CAT_*) to its string name. Used by
// the SYSTEM tab / command browser to honour the three-tier model.
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

// `label` — the inspected node's display label. Falls back
// to its path-segment name when no explicit label was set.
static value_t meta_get_label(struct object *self, const member_t *m) {
    (void)m;
    struct object *insp = meta_inspected(self);
    const char *label = object_label(insp);
    return val_str(label ? label : "");
}

// `category` — the inspected node's own visibility tier (basic / advanced /
// internal). Lets the SYSTEM tab decide whether to show a child object
// without a separate allowlist.
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
    // `line` is a required V_STRING and `cursor` a V_INT, so node_validate_args
    // has already rejected a call that does not supply them that way.  Only
    // `cursor` being absent is still a live case -- it is optional with no
    // default, so a one-argument call truncates argc to 1.
    const char *line = argv[0].s;
    int cursor = (argc >= 2 && argv[1].kind == V_INT) ? (int)argv[1].i : (int)strlen(line);
    if (!g_complete_provider)
        return val_list(NULL, 0);
    return g_complete_provider(line, cursor);
}

// `member(name)` — short text description of one named member. Cheaper
// than enumerating the full attributes/methods list when the caller
// already knows which member it wants.
static value_t meta_method_member(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc; // the declared arg table guarantees argv[0] is a non-empty string
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
        kind_str = !mb->attr.set ? "attribute (read-only)" : "attribute";
        break;
    case M_METHOD:
        kind_str = "method";
        break;
    case M_CHILD:
        kind_str = member_is_collection(mb) ? "child[]" : "child";
        break;
    }
    char buf[320];
    snprintf(buf, sizeof(buf), "%s: %s%s%s", mb->name ? mb->name : "?", kind_str, mb->doc ? " — " : "",
             mb->doc ? mb->doc : "");
    return val_str(buf);
}

// `member_category(name)` — visibility tier of a named member on the
// inspected class: "basic" / "advanced" / "internal". The
// SYSTEM tab reads this to decide whether to show an attribute/method row.
// Unknown names default to "basic" (faithful-by-default).
static value_t meta_method_member_category(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc; // the declared arg table guarantees argv[0] is a non-empty string
    struct object *insp = meta_inspected(self);
    const member_t *mb = class_find_member(insp ? object_class(insp) : NULL, argv[0].s);
    return val_str(category_name(mb ? mb->flags : 0));
}

// `member_label(name)` — display label of a named member, falling back to the
// name itself.
static value_t meta_method_member_label(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc; // the declared arg table guarantees argv[0] is a non-empty string
    struct object *insp = meta_inspected(self);
    const member_t *mb = class_find_member(insp ? object_class(insp) : NULL, argv[0].s);
    const char *label = (mb && mb->label) ? mb->label : argv[0].s;
    return val_str(label);
}

// `method_info(name)` — UI metadata for a method member, a typed map so the
// context menu and command browser render it without a static catalogue:
// verb label, destructive/mutate/hidden/io flags, declared arg
// count, and doc. Returns a V_ERROR if the member
// is not a method.
static value_t meta_method_method_info(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc; // the declared arg table guarantees argv[0] is a non-empty string
    struct object *insp = meta_inspected(self);
    const member_t *mb = class_find_member(insp ? object_class(insp) : NULL, argv[0].s);
    if (!mb || mb->kind != M_METHOD)
        return val_err("method_info: '%s' is not a method", argv[0].s);
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "name", val_str(mb->name ? mb->name : ""));
    val_map_put(b, "verb", val_str(mb->method.verb_label ? mb->method.verb_label : (mb->name ? mb->name : "")));
    val_map_put(b, "category", val_str(category_name(mb->flags)));
    val_map_put(b, "doc", val_str(mb->doc ? mb->doc : ""));
    val_map_put(b, "destructive", val_bool((mb->method.ui_flags & MM_DESTRUCTIVE) != 0));
    val_map_put(b, "mutate", val_bool((mb->method.ui_flags & MM_MUTATE) != 0));
    val_map_put(b, "hidden", val_bool((mb->method.ui_flags & MM_HIDDEN) != 0));
    val_map_put(b, "io", val_bool((mb->method.ui_flags & MM_IO) != 0));
    val_map_put(b, "nargs", val_int((int64_t)mb->method.nargs));
    return val_map_finish(b);
}

// `indices(name)` — the live indices of an indexed-child member. Lets a
// tree walker enumerate a sparse collection's occupants
// (machine.scsi.device[0], [3], …) instead of stopping at the bare collection
// member. Returns a V_LIST<V_INT> for an indexed member (possibly empty), or
// a V_ERROR for a non-indexed / unknown member — so a caller can use the
// error/list distinction to tell "indexed collection" from "named child".
static value_t indices_of(struct object *insp, const member_t *mb);

static value_t meta_method_indices(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc; // the declared arg table guarantees argv[0] is a non-empty string
    struct object *insp = meta_inspected(self);
    const member_t *mb = class_find_member(insp ? object_class(insp) : NULL, argv[0].s);
    if (!member_is_collection(mb))
        return val_err("indices: '%s' is not an indexed child", argv[0].s);
    return indices_of(insp, mb);
}

// `members(values?)` — every member of the inspected node in one call, one
// map each: {name, kind: attr|child|method, category, label, doc}, plus
// `readonly` (and, with values=true, `value`) for an attribute, `indexed`
// (and `indices`) for a child, and the method_info fields for a method.
// Runtime-attached children are listed after the class members with their
// own label and category.  A tree view used to spend two or three round
// trips per member on member_category / member_label / indices.
// Values are opt-in: some attributes are volatile or costly to read.

// The live indices of an indexed-child member, as a V_LIST<V_INT>.
static value_t indices_of(struct object *insp, const member_t *mb) {
    value_t *items = NULL;
    size_t len = 0, cap = 0;
    for (int i = object_child_next(insp, mb, -1); i >= 0; i = object_child_next(insp, mb, i)) {
        if (!val_list_push(&items, &len, &cap, val_int(i))) {
            for (size_t k = 0; k < len; k++)
                value_free(&items[k]);
            free(items);
            return val_err("out of memory");
        }
    }
    return val_list(items, len);
}

// === Type descriptors (§ type descriptor) ==================================
//
// {kind, width, presentation, enum}: what a value of this slot is, for
// editors, argument forms, usage text and completion.

const char *meta_presentation_text(uint16_t flags) {
    if (flags & VAL_SENSITIVE)
        return "sensitive";
    if (flags & VAL_PATH)
        return "path";
    if (flags & VAL_HEX)
        return "hex";
    if (flags & VAL_BIN)
        return "bin";
    if (flags & VAL_DEC)
        return "dec";
    return NULL;
}

value_t meta_type_descriptor(value_kind_t kind, uint8_t width, uint16_t presentation, const char *const *enum_values) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "kind", val_str(value_kind_name(kind)));
    val_map_put(b, "width", val_uint(1, width));
    const char *pres = meta_presentation_text(presentation);
    val_map_put(b, "presentation", pres ? val_str(pres) : val_none());
    if (enum_values && enum_values[0]) {
        value_t *items = NULL;
        size_t len = 0, cap = 0;
        for (size_t i = 0; enum_values[i]; i++)
            val_list_push(&items, &len, &cap, val_str(enum_values[i]));
        val_map_put(b, "enum", val_list(items, len));
    } else {
        val_map_put(b, "enum", val_none());
    }
    return val_map_finish(b);
}

// The live keys of a keyed collection member, as a V_LIST<V_STRING>, or none.
static value_t keys_of(struct object *insp, const member_t *mb) {
    if (!mb->child.collection->by_key.next_key)
        return val_none();
    value_t *items = NULL;
    size_t len = 0, cap = 0;
    for (const char *k = object_child_next_key(insp, mb, NULL); k; k = object_child_next_key(insp, mb, k))
        val_list_push(&items, &len, &cap, val_str(k));
    return val_list(items, len);
}

// Whether a collection hands out entries by index.
static bool by_index(const member_t *mb) {
    return mb->child.collection->by_index.get != NULL;
}

// Put the collection keys of a child that is (or is not) a collection
// container: `collection`, and for a container `indices` / `keys`.
static void put_collection(value_map_builder_t *b, struct object *child) {
    const member_t *entries = child ? class_collection(object_class(child)) : NULL;
    val_map_put(b, "collection", val_bool(entries != NULL));
    if (!entries)
        return;
    val_map_put(b, "indices", by_index(entries) ? indices_of(child, entries) : val_none());
    val_map_put(b, "keys", keys_of(child, entries));
}

static value_t describe_member(struct object *insp, const member_t *mb, bool values) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "name", val_str(mb->name ? mb->name : ""));
    const char *kind = mb->kind == M_ATTR ? "attr" : mb->kind == M_CHILD ? "child" : "method";
    val_map_put(b, "kind", val_str(kind));
    val_map_put(b, "category", val_str(category_name(mb->flags)));
    val_map_put(b, "label", val_str(mb->label ? mb->label : (mb->name ? mb->name : "")));
    struct object *child = NULL;
    if (mb->kind == M_CHILD && !mb->child.reference)
        child = object_named_child(insp, mb);
    const char *doc = mb->doc ? mb->doc : "";
    if (mb->kind == M_CHILD && !*doc && child)
        doc = object_doc(child);
    val_map_put(b, "doc", val_str(doc));
    switch (mb->kind) {
    case M_ATTR:
        val_map_put(b, "readonly", val_bool(!mb->attr.set));
        val_map_put(
            b, "type",
            meta_type_descriptor(mb->attr.type, mb->attr.width, mb->attr.presentation_flags, mb->attr.enum_values));
        if (values)
            val_map_put(b, "value", node_get((node_t){.obj = insp, .member = mb, .index = -1}));
        break;
    case M_CHILD:
        val_map_put(b, "indexed", val_bool(member_is_collection(mb)));
        if (member_is_collection(mb)) {
            val_map_put(b, "indices", by_index(mb) ? indices_of(insp, mb) : val_none());
            val_map_put(b, "keys", keys_of(insp, mb));
            val_map_put(b, "collection", val_bool(false));
        } else {
            put_collection(b, child);
        }
        break;
    case M_METHOD: {
        val_map_put(b, "verb", val_str(mb->method.verb_label ? mb->method.verb_label : (mb->name ? mb->name : "")));
        val_map_put(b, "destructive", val_bool((mb->method.ui_flags & MM_DESTRUCTIVE) != 0));
        val_map_put(b, "mutate", val_bool((mb->method.ui_flags & MM_MUTATE) != 0));
        val_map_put(b, "hidden", val_bool((mb->method.ui_flags & MM_HIDDEN) != 0));
        val_map_put(b, "io", val_bool((mb->method.ui_flags & MM_IO) != 0));
        val_map_put(b, "nargs", val_int((int64_t)mb->method.nargs));
        value_t *args = NULL;
        size_t len = 0, cap = 0;
        for (int i = 0; i < mb->method.nargs && mb->method.args; i++) {
            const arg_decl_t *a = &mb->method.args[i];
            value_map_builder_t *ab = val_map_new();
            val_map_put(ab, "name", val_str(a->name ? a->name : ""));
            char doc[512];
            arg_doc_text(a, doc, sizeof(doc));
            val_map_put(ab, "doc", val_str(doc));
            val_map_put(ab, "type", meta_type_descriptor(a->kind, a->width, a->presentation_flags, a->enum_values));
            val_map_put(ab, "optional", val_bool((a->validation_flags & OBJ_ARG_OPTIONAL) != 0));
            val_map_put(ab, "rest", val_bool((a->validation_flags & OBJ_ARG_REST) != 0));
            val_map_put(ab, "default", arg_has_default(a) ? value_dup(a->default_value) : val_none());
            val_list_push(&args, &len, &cap, val_map_finish(ab));
        }
        val_map_put(b, "args", val_list(args, len));
        val_map_put(b, "result", meta_type_descriptor(mb->method.result, 0, 0, NULL));
        if (mb->method.result_doc)
            val_map_put(b, "result_doc", val_str(mb->method.result_doc));
        break;
    }
    }
    if (mb->examples && mb->examples[0]) {
        value_t *ex = NULL;
        size_t len = 0, cap = 0;
        for (size_t i = 0; mb->examples[i]; i++)
            val_list_push(&ex, &len, &cap, val_str(mb->examples[i]));
        val_map_put(b, "examples", val_list(ex, len));
    }
    return val_map_finish(b);
}

typedef struct {
    value_t *items;
    size_t len, cap;
    bool oom;
} member_list_t;

static void member_list_push(member_list_t *acc, value_t v) {
    if (v.kind == V_ERROR) {
        value_free(&v);
        acc->oom = true;
        return;
    }
    if (!val_list_push(&acc->items, &acc->len, &acc->cap, v))
        acc->oom = true;
}

static void each_attached_describe(struct object *parent, struct object *child, void *ud) {
    const char *name = object_name(child);
    if (!name)
        return;
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "name", val_str(name));
    val_map_put(b, "kind", val_str("child"));
    val_map_put(b, "category", val_str(category_name(object_category(child))));
    const char *label = object_label(child);
    val_map_put(b, "label", val_str(label ? label : name));
    val_map_put(b, "doc", val_str(object_doc(child)));
    if (!object_parent(parent)) // a root child: its domain
        val_map_put(b, "domain", val_str(object_domain_name(object_domain(child))));
    val_map_put(b, "indexed", val_bool(false));
    put_collection(b, child);
    member_list_push((member_list_t *)ud, val_map_finish(b));
}

static value_t meta_method_members(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    bool values = argc >= 1 && argv[0].kind == V_BOOL && argv[0].b;
    struct object *insp = meta_inspected(self);
    if (!insp)
        return val_list(NULL, 0);
    member_list_t acc = {0};
    const class_desc_t *cls = object_class(insp);
    for (size_t i = 0; cls && i < cls->n_members && !acc.oom; i++)
        member_list_push(&acc, describe_member(insp, &cls->members[i], values));
    if (!acc.oom)
        object_each_attached_ordered(insp, each_attached_describe, &acc);
    if (acc.oom) {
        for (size_t i = 0; i < acc.len; i++)
            value_free(&acc.items[i]);
        free(acc.items);
        return val_err("out of memory");
    }
    return val_list(acc.items, acc.len);
}

// === Class table =========================================================

static const arg_decl_t meta_complete_args[] = {
    {.name = "line", .kind = V_STRING, .doc = "Input line to complete"},
    {.name = "cursor",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Cursor position in line",
     .default_doc = "the end of the line"},
};

static const arg_decl_t meta_member_args[] = {
    {.name = "name",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_NONEMPTY,
     .doc = "Member name on the inspected class"},
};

static const arg_decl_t meta_named_member_args[] = {
    {.name = "name", .kind = V_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "Member name"},
};

static const arg_decl_t meta_members_args[] = {
    {.name = "values",
     .kind = V_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Also read each attribute's current value"},
};

static const member_t meta_members[] = {
    {.kind = M_ATTR,
     .name = "class",
     .doc = "Class name of the inspected node",
     .attr = {.type = V_STRING, .get = meta_get_class, .set = NULL}                                                                                                                         },
    {.kind = M_ATTR,
     .name = "doc",
     .doc = "One-sentence description of the inspected node (its own doc, else its class's)",
     .attr = {.type = V_STRING, .get = meta_get_doc, .set = NULL}                                                                                                                           },
    {.kind = M_ATTR,
     .name = "path",
     .doc = "Absolute dotted path of the inspected node",
     .attr = {.type = V_STRING, .get = meta_get_path, .set = NULL}                                                                                                                          },
    {.kind = M_ATTR,
     .name = "label",
     .doc = "Human-facing display label of the inspected node (falls back to name)",
     .attr = {.type = V_STRING, .get = meta_get_label, .set = NULL}                                                                                                                         },
    {.kind = M_ATTR,
     .name = "category",
     .doc = "Visibility tier of the inspected node: basic | advanced | internal",
     .attr = {.type = V_STRING, .get = meta_get_category, .set = NULL}                                                                                                                      },
    {.kind = M_ATTR,
     .name = "children",
     .doc = "Names of sub-objects on the inspected node",
     .attr = {.type = V_LIST, .get = meta_get_children, .set = NULL}                                                                                                                        },
    {.kind = M_ATTR,
     .name = "attributes",
     .doc = "Names of attribute members on the inspected class",
     .attr = {.type = V_LIST, .get = meta_get_attributes, .set = NULL}                                                                                                                      },
    {.kind = M_ATTR,
     .name = "methods",
     .doc = "Names of method members on the inspected class",
     .attr = {.type = V_LIST, .get = meta_get_methods, .set = NULL}                                                                                                                         },
    {.kind = M_METHOD,
     .name = "complete",
     .doc = "Tab-completion candidates for a partial line",
     .method = {.args = meta_complete_args, .nargs = 2, .result = V_LIST, .fn = meta_method_complete}                                                                                       },
    {.kind = M_METHOD,
     .name = "member",
     .doc = "Short description of one named member",
     .method = {.args = meta_member_args, .nargs = 1, .result = V_STRING, .fn = meta_method_member}                                                                                         },
    {.kind = M_METHOD,
     .name = "member_category",
     .doc = "Visibility tier of a named member: basic | advanced | internal",
     .method = {.args = meta_named_member_args, .nargs = 1, .result = V_STRING, .fn = meta_method_member_category}                                                                          },
    {.kind = M_METHOD,
     .name = "member_label",
     .doc = "Display label of a named member (falls back to its name)",
     .method = {.args = meta_named_member_args, .nargs = 1, .result = V_STRING, .fn = meta_method_member_label}                                                                             },
    {.kind = M_METHOD,
     .name = "method_info",
     .doc = "JSON UI metadata for a method (verb, destructive, mutate, hidden, nargs)",
     .method = {.args = meta_named_member_args, .nargs = 1, .result = V_MAP, .fn = meta_method_method_info}                                                                                 },
    {.kind = M_METHOD,
     .name = "members",
     .doc = "Every member in one call: name, kind, category, label, doc, and per kind readonly/value, "
            "indexed/indices or the method_info fields",                                      .method = {.args = meta_members_args, .nargs = 1, .result = V_LIST, .fn = meta_method_members}},
    {.kind = M_METHOD,
     .name = "indices",
     .doc = "Live indices of an indexed-child member (errors if not indexed)",
     .method = {.args = meta_named_member_args, .nargs = 1, .result = V_LIST, .fn = meta_method_indices}                                                                                    },
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
