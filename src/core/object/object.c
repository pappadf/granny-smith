// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// object.c
// Object-model substrate. See object.h for the contract.

#include "object.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "meta.h"

// === Object representation ==================================================
//
// Objects form a tree. Each object holds a pointer to its parent and the
// head of a singly-linked list of attached children, threaded via
// next_sibling. Attach is O(1) (push at head); detach is O(N) over the
// parent's child list, which is fine for the small fan-outs we expect
// (≤ tens). Indexed children are not stored here — they are produced on
// demand by the class's member descriptor callbacks.

// Listener for object_fire_invalidators. Stored as a small linked list
// off the object; size is bounded by the number of hot-path consumers
// that hold pre-resolved nodes targeting this object (≤ tens in practice).
struct invalidator {
    node_invalidate_fn cb;
    void *ud;
    struct invalidator *next;
};

struct object {
    const class_desc_t *cls;
    void *instance_data;
    const char *name;
    struct object *parent;
    struct object *first_child;
    struct object *next_sibling;
    struct invalidator *invalidators; // weak-ref callbacks for held nodes
    struct object *meta_node; // lazily-created Meta node bound to this object (see meta.c)
};

struct object *object_get_meta(struct object *o) {
    return o ? o->meta_node : NULL;
}

void object_set_meta(struct object *o, struct object *meta) {
    if (o)
        o->meta_node = meta;
}

// Root class: namespace-only by default. root_install swaps in a
// richer class via object_root_set_class() to register the top-level
// methods.
static const class_desc_t emu_root_class_default = {
    .name = "emu",
    .members = NULL,
    .n_members = 0,
};

static struct object *g_root = NULL;

struct object *object_root(void) {
    if (!g_root)
        g_root = object_new(&emu_root_class_default, NULL, "emu");
    return g_root;
}

void object_root_set_class(const class_desc_t *cls) {
    struct object *root = object_root();
    if (!root)
        return;
    root->cls = cls ? cls : &emu_root_class_default;
}

void object_root_reset(void) {
    if (!g_root)
        return;
    // Detach every child first so children's parent pointers are cleared
    // before we free the root. Callers own the children themselves.
    while (1) {
        struct object *c = NULL;
        for (struct object *it = g_root->first_child; it; it = it->next_sibling) {
            c = it;
            break;
        }
        if (!c)
            break;
        object_detach(c);
    }
    // Free the cached Meta node on the root, if any. Callers leak the
    // synthetic introspection node otherwise — object_delete is the only
    // path that runs meta_node_release, and the root bypasses it here.
    meta_node_release(g_root);
    free(g_root);
    g_root = NULL;
}

struct object *object_new(const class_desc_t *cls, void *instance_data, const char *name) {
    if (!cls)
        return NULL;
    struct object *o = (struct object *)calloc(1, sizeof(*o));
    if (!o)
        return NULL;
    o->cls = cls;
    o->instance_data = instance_data;
    o->name = name;
    return o;
}

void object_delete(struct object *o) {
    if (!o)
        return;
    // Fire invalidators before tearing down — listeners must drop their
    // cached node_t while the object is still inspectable, but they
    // must not dereference it after this returns.
    object_fire_invalidators(o);
    // Free the synthetic Meta node (if any) before freeing self so a
    // cached meta_node cannot outlive its inspected target. The release
    // helper short-circuits when there is nothing cached.
    meta_node_release(o);
    if (o->parent)
        object_detach(o);
    free(o);
}

void object_register_invalidator(struct object *o, node_invalidate_fn cb, void *ud) {
    if (!o || !cb)
        return;
    struct invalidator *inv = (struct invalidator *)calloc(1, sizeof(*inv));
    if (!inv)
        return;
    inv->cb = cb;
    inv->ud = ud;
    // Append to keep firing order = registration order.
    struct invalidator **link = &o->invalidators;
    while (*link)
        link = &(*link)->next;
    *link = inv;
}

void object_unregister_invalidator(struct object *o, node_invalidate_fn cb, void *ud) {
    if (!o || !cb)
        return;
    struct invalidator **link = &o->invalidators;
    while (*link) {
        if ((*link)->cb == cb && (*link)->ud == ud) {
            struct invalidator *dead = *link;
            *link = dead->next;
            free(dead);
            return;
        }
        link = &(*link)->next;
    }
}

void object_fire_invalidators(struct object *o) {
    if (!o)
        return;
    struct invalidator *list = o->invalidators;
    o->invalidators = NULL; // clear first so callbacks can re-register safely
    while (list) {
        struct invalidator *next = list->next;
        if (list->cb)
            list->cb(list->ud);
        free(list);
        list = next;
    }
}

void object_attach(struct object *parent, struct object *child) {
    if (!parent || !child)
        return;
    GS_ASSERTF(child->parent == NULL, "object %s already attached", child->name ? child->name : "?");
    child->parent = parent;
    child->next_sibling = parent->first_child;
    parent->first_child = child;
}

void object_detach(struct object *child) {
    if (!child || !child->parent)
        return;
    struct object *parent = child->parent;
    struct object **link = &parent->first_child;
    while (*link && *link != child)
        link = &(*link)->next_sibling;
    if (*link == child)
        *link = child->next_sibling;
    child->parent = NULL;
    child->next_sibling = NULL;
}

const class_desc_t *object_class(const struct object *o) {
    return o ? o->cls : NULL;
}
const char *object_name(const struct object *o) {
    return o ? o->name : NULL;
}
void *object_data(struct object *o) {
    return o ? o->instance_data : NULL;
}
struct object *object_parent(struct object *o) {
    return o ? o->parent : NULL;
}

void object_each_attached(struct object *o, void (*fn)(struct object *parent, struct object *child, void *ud),
                          void *ud) {
    if (!o || !fn)
        return;
    for (struct object *c = o->first_child; c; c = c->next_sibling)
        fn(o, c, ud);
}

const member_t *class_find_member(const class_desc_t *cls, const char *name) {
    if (!cls || !name || !cls->members)
        return NULL;
    for (size_t i = 0; i < cls->n_members; i++) {
        const member_t *m = &cls->members[i];
        if (m->name && strcmp(m->name, name) == 0)
            return m;
    }
    return NULL;
}

// Look up a statically-attached child by name (one of the parent's
// linked children). Used when the class declares a named child without
// providing its own lookup callback.
static struct object *find_attached_child(struct object *parent, const char *name) {
    if (!parent || !name)
        return NULL;
    for (struct object *c = parent->first_child; c; c = c->next_sibling)
        if (c->name && strcmp(c->name, name) == 0)
            return c;
    return NULL;
}

// === Reserved words / name validation =======================================
//
// One closed list. Match string equality (case-sensitive) — identifiers
// are case-sensitive everywhere else in the codebase. This list mirrors
// proposal-module-object-model.md §2.3.

static const char *const RESERVED_WORDS[] = {
    // Boolean literal spellings.
    "true",
    "false",
    "on",
    "off",
    "yes",
    "no",
    // Script-grammar keywords.
    "let",
    "if",
    "else",
    "while",
    // Future-reserved.
    "for",
    "do",
    "return",
    "break",
    "continue",
};

bool object_is_reserved_word(const char *name) {
    if (!name)
        return false;
    for (size_t i = 0; i < sizeof(RESERVED_WORDS) / sizeof(RESERVED_WORDS[0]); i++)
        if (strcmp(name, RESERVED_WORDS[i]) == 0)
            return true;
    return false;
}

// Pure identifier per §2.3: [A-Za-z_][A-Za-z0-9_]*
static bool is_valid_identifier(const char *name) {
    if (!name || !*name)
        return false;
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_'))
        return false;
    for (const char *p = name + 1; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_'))
            return false;
    }
    return true;
}

bool object_validate_name(const char *name, char *err_buf, size_t err_size) {
    if (!is_valid_identifier(name)) {
        if (err_buf && err_size)
            snprintf(err_buf, err_size, "not a valid identifier: '%s'", name ? name : "(null)");
        return false;
    }
    if (object_is_reserved_word(name)) {
        if (err_buf && err_size)
            snprintf(err_buf, err_size, "'%s' is a reserved word", name);
        return false;
    }
    return true;
}

// Forward declaration: defined in the validator section below.
static const char *kind_name(value_kind_t k);

// Helpers for §3.13 ordering / coercion checks.
static bool kind_supports_width(value_kind_t k) {
    return k == V_INT || k == V_UINT;
}
static bool kind_supports_strict(value_kind_t k) {
    // Strict-kind only meaningful for kinds that have coercion paths.
    return k == V_INT || k == V_UINT || k == V_BOOL || k == V_FLOAT || k == V_ENUM;
}

bool object_validate_class(const class_desc_t *cls, char *err_buf, size_t err_size) {
    if (!cls) {
        if (err_buf && err_size)
            snprintf(err_buf, err_size, "class is NULL");
        return false;
    }
    if (!cls->name || !is_valid_identifier(cls->name)) {
        if (err_buf && err_size)
            snprintf(err_buf, err_size, "invalid class name");
        return false;
    }
    for (size_t i = 0; i < cls->n_members; i++) {
        const member_t *m = &cls->members[i];
        char sub_err[160];
        if (!object_validate_name(m->name, sub_err, sizeof(sub_err))) {
            if (err_buf && err_size)
                snprintf(err_buf, err_size, "class %s member[%zu]: %s", cls->name, i, sub_err);
            return false;
        }
        // `meta` is reserved for the synthetic introspection node — see
        // proposal-introspection-via-meta-attribute.md §2.1.
        if (m->name && strcmp(m->name, "meta") == 0) {
            if (err_buf && err_size)
                snprintf(err_buf, err_size, "class %s: 'meta' is reserved for introspection", cls->name);
            return false;
        }
        // Duplicate-name check within the class.
        for (size_t j = 0; j < i; j++) {
            if (cls->members[j].name && strcmp(cls->members[j].name, m->name) == 0) {
                if (err_buf && err_size)
                    snprintf(err_buf, err_size, "class %s: duplicate member '%s'", cls->name, m->name);
                return false;
            }
        }

        // Method-arg ordering / coercion invariants (proposal §3.13).
        if (m->kind == M_METHOD && m->method.args && m->method.nargs > 0) {
            const arg_decl_t *args = m->method.args;
            int nargs = m->method.nargs;
            bool seen_optional = false;
            int rest_count = 0;
            for (int a = 0; a < nargs; a++) {
                const arg_decl_t *p = &args[a];
                bool is_opt = (p->validation_flags & OBJ_ARG_OPTIONAL) != 0;
                bool is_rest = (p->validation_flags & OBJ_ARG_REST) != 0;
                if (is_rest) {
                    rest_count++;
                    if (a != nargs - 1) {
                        if (err_buf && err_size)
                            snprintf(err_buf, err_size, "%s.%s: rest arg '%s' must be the last parameter", cls->name,
                                     m->name, p->name ? p->name : "?");
                        return false;
                    }
                    if (is_opt) {
                        if (err_buf && err_size)
                            snprintf(err_buf, err_size, "%s.%s: rest arg '%s' must not also be optional", cls->name,
                                     m->name, p->name ? p->name : "?");
                        return false;
                    }
                }
                if (is_opt)
                    seen_optional = true;
                if (!is_opt && !is_rest && seen_optional) {
                    if (err_buf && err_size)
                        snprintf(err_buf, err_size, "%s.%s: required arg '%s' follows optional", cls->name, m->name,
                                 p->name ? p->name : "?");
                    return false;
                }
                if (p->default_value && !is_opt) {
                    if (err_buf && err_size)
                        snprintf(err_buf, err_size, "%s.%s: arg '%s' has default but is not optional", cls->name,
                                 m->name, p->name ? p->name : "?");
                    return false;
                }
                if (p->default_value && p->default_value->kind != p->kind) {
                    if (err_buf && err_size)
                        snprintf(err_buf, err_size, "%s.%s: default for arg '%s' is %s, declared %s", cls->name,
                                 m->name, p->name ? p->name : "?", kind_name(p->default_value->kind),
                                 kind_name(p->kind));
                    return false;
                }
                if (p->kind == V_ENUM && (!p->enum_values || !p->enum_values[0])) {
                    if (err_buf && err_size)
                        snprintf(err_buf, err_size, "%s.%s: arg '%s' is V_ENUM but has no enum_values table", cls->name,
                                 m->name, p->name ? p->name : "?");
                    return false;
                }
                if (p->width && !kind_supports_width(p->kind)) {
                    // Warning: width meaningless for this kind. Logged but
                    // not fatal. Stay quiet by default — class authors will
                    // see it via a follow-up audit pass.
                    (void)0;
                }
                (void)kind_supports_strict;
            }
            if (rest_count > 1) {
                if (err_buf && err_size)
                    snprintf(err_buf, err_size, "%s.%s: only one rest arg allowed", cls->name, m->name);
                return false;
            }
        }

        // Attribute-slot invariants.
        if (m->kind == M_ATTR) {
            if (m->attr.validation_flags & (OBJ_ARG_OPTIONAL | OBJ_ARG_REST)) {
                if (err_buf && err_size)
                    snprintf(err_buf, err_size, "%s.%s: arg-only flag set on attribute slot", cls->name, m->name);
                return false;
            }
            if (m->attr.type == V_ENUM && (!m->attr.enum_values || !m->attr.enum_values[0])) {
                if (err_buf && err_size)
                    snprintf(err_buf, err_size, "%s.%s: V_ENUM attribute slot has no enum_values table", cls->name,
                             m->name);
                return false;
            }
        }
    }
    return true;
}

// === Path resolution =========================================================
//
// A path is a sequence of segments separated by '.'. A segment is one of:
//   identifier     resolves a named member or named child
//   integer        sugar for an indexed child of the current node
//   [integer]      explicit index form (index brackets close back into the
//                   same path; the closing ']' is consumed)
//
// Method-call surface forms (`step(1000)` / `step 1000`) are not
// recognised here — methods resolve as nodes; the caller invokes them
// via node_call.

// Skip leading whitespace.
static const char *skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p))
        p++;
    return p;
}

// Try to read an integer (decimal, 0x.. hex, or 0b.. binary). On success
// writes the value into *out and returns the position after the digits.
// Returns NULL if no integer was recognised at *p.
static const char *parse_int(const char *p, long long *out) {
    if (!p)
        return NULL;
    const char *start = p;
    int base = 10;
    int sign = 1;
    if (*p == '-') {
        sign = -1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        base = 2;
        p += 2;
    }
    char *endp = NULL;
    long long v = strtoll(p, &endp, base);
    if (!endp || endp == p)
        return NULL;
    if (out)
        *out = sign * v;
    (void)start;
    return endp;
}

// Try to read an identifier. On success copies up to buf_size-1 chars
// into buf and returns the position after the identifier. Returns NULL
// if no identifier was recognised at *p.
static const char *parse_ident(const char *p, char *buf, size_t buf_size) {
    if (!p || !buf || buf_size == 0)
        return NULL;
    if (!(isalpha((unsigned char)*p) || *p == '_'))
        return NULL;
    size_t i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i + 1 < buf_size)
            buf[i++] = *p;
        p++;
    }
    buf[i] = '\0';
    return p;
}

node_t node_child(node_t n, const char *segment) {
    node_t bad = (node_t){0};
    if (!segment || !*segment)
        return bad;
    if (!n.obj)
        return bad;

    // Attribute / method nodes are leaves — no further descent.
    if (n.member && n.member->kind != M_CHILD)
        return bad;

    // Probe for a pure-integer segment up front. Integer segments can
    // either *select an index* on a pending indexed-child member
    // (`devices[0]`, `devices.0`) or *enter* the first indexed-child
    // member of the current object (`bucket.0` shorthand for
    // `bucket.<first-indexed>.0`). They look the same syntactically.
    bool is_int = false;
    long long ival = 0;
    {
        char *endp = NULL;
        long v = strtol(segment, &endp, 0);
        if (endp && endp != segment && *endp == '\0') {
            is_int = true;
            ival = v;
        }
    }

    // Case 1: `n` is sitting on an indexed-child member with no index
    // chosen yet. An integer segment supplies that index. This is what
    // makes both `bucket.devices[0]` and `bucket.devices.0` work.
    if (n.member && n.member->kind == M_CHILD && n.member->child.indexed && n.index < 0 && is_int)
        return (node_t){.obj = n.obj, .member = n.member, .index = (int)ival};

    // Determine the "current object" we descend from. If `n` already
    // points at a fully-resolved child slot or a named-child member,
    // descend into the target object first, then resolve the segment
    // against that target.
    struct object *here = n.obj;
    if (n.member && n.member->kind == M_CHILD) {
        if (n.member->child.indexed) {
            if (n.index < 0)
                return bad; // case 1 above already handled int segments
            if (!n.member->child.get)
                return bad;
            struct object *child = n.member->child.get(n.obj, n.index);
            if (!child)
                return bad;
            here = child;
        } else {
            struct object *child = NULL;
            if (n.member->child.lookup)
                child = n.member->child.lookup(n.obj, n.member->name);
            if (!child)
                child = find_attached_child(n.obj, n.member->name);
            if (!child)
                return bad;
            here = child;
        }
    }

    // Synthetic `meta` segment (proposal-introspection-via-meta-attribute.md).
    // Every object implicitly carries a `meta` attribute whose value is a
    // Meta node bound to it. The segment intercept lives here — after the
    // M_CHILD descent computes the real target object, before the regular
    // member lookup — so paths like `cpu.meta`, `floppy.drives.0.meta`,
    // and bare `meta` (root) all resolve uniformly.
    if (strcmp(segment, "meta") == 0) {
        struct object *meta = meta_node_for(here);
        if (!meta)
            return bad;
        return (node_t){.obj = meta, .member = NULL, .index = -1};
    }

    // Integer segment now means "first indexed-child member of `here`."
    // Conventionally a class declares at most one such member (e.g.
    // `scsi.devices`), so taking the first match matches user intent.
    if (is_int) {
        const class_desc_t *cls = object_class(here);
        if (!cls)
            return bad;
        for (size_t i = 0; i < cls->n_members; i++) {
            const member_t *m = &cls->members[i];
            if (m->kind == M_CHILD && m->child.indexed)
                return (node_t){.obj = here, .member = m, .index = (int)ival};
        }
        return bad;
    }

    // Identifier segment → named member of here's class.
    const class_desc_t *cls = object_class(here);
    const member_t *m = class_find_member(cls, segment);
    if (m)
        return (node_t){.obj = here, .member = m, .index = -1};

    // Identifier may also name a statically-attached child object the
    // class did not predeclare as a member (the root uses this — its
    // children are added via object_attach at runtime).
    struct object *child = find_attached_child(here, segment);
    if (child)
        return (node_t){.obj = child, .member = NULL, .index = -1};

    return bad;
}

node_t object_resolve(struct object *root, const char *path) {
    node_t bad = (node_t){0};
    if (!root)
        return bad;
    node_t cur = (node_t){.obj = root, .member = NULL, .index = -1};
    if (!path || !*path)
        return cur;

    const char *p = skip_ws(path);
    while (*p) {
        // Bracket form: [INT]
        if (*p == '[') {
            long long idx = 0;
            p = skip_ws(p + 1);
            const char *q = parse_int(p, &idx);
            if (!q)
                return bad;
            q = skip_ws(q);
            if (*q != ']')
                return bad;
            char seg[32];
            snprintf(seg, sizeof(seg), "%lld", idx);
            cur = node_child(cur, seg);
            if (!node_valid(cur))
                return bad;
            p = skip_ws(q + 1);
            // Optional leading '.' before the next segment.
            if (*p == '.')
                p = skip_ws(p + 1);
            continue;
        }

        // Identifier or integer segment.
        if (isdigit((unsigned char)*p) || *p == '-' || *p == '+') {
            long long idx = 0;
            const char *q = parse_int(p, &idx);
            if (!q)
                return bad;
            char seg[32];
            snprintf(seg, sizeof(seg), "%lld", idx);
            cur = node_child(cur, seg);
            if (!node_valid(cur))
                return bad;
            p = skip_ws(q);
        } else {
            char ident[128];
            const char *q = parse_ident(p, ident, sizeof(ident));
            if (!q || q == p)
                return bad;
            cur = node_child(cur, ident);
            if (!node_valid(cur))
                return bad;
            p = skip_ws(q);
        }
        // Separator: '.' or '[' or end-of-path.
        if (*p == '.')
            p = skip_ws(p + 1);
        else if (*p == '[')
            continue;
        else if (*p == '\0')
            break;
        else
            return bad;
    }
    return cur;
}

// === Argument / setter validation ===========================================
//
// Single engine drives both: arg_decl_t (one per method param) and
// member.attr (one per attribute) project onto the same `typed_slot_t`
// view, then validate_slot() enforces kind / width / non-empty / enum
// rules with limited coercion. See proposal-typed-dispatch.md §3.

#define OBJ_VALIDATE_MAX_ARGS 16

typedef struct typed_slot {
    const char *name; // arg name; "" for attribute slots
    value_kind_t kind;
    uint8_t width; // 1/2/4/8 for V_INT/V_UINT range; 0 = unconstrained
    unsigned flags; // OBJ_ARG_OPTIONAL | OBJ_ARG_REST | OBJ_ARG_NONEMPTY | OBJ_ARG_STRICT_KIND
    const char *const *enum_values; // NULL-terminated; required if kind == V_ENUM
    const value_t *default_value; // optional default for OBJ_ARG_OPTIONAL
} typed_slot_t;

typedef enum {
    VALIDATE_OK = 0, // input passes, no rewrite needed
    VALIDATE_REWRITE, // input passes after coercion; rewritten value in *out
    VALIDATE_ERR, // validation failed; message in err_buf
} validate_status_t;

// Project an arg_decl onto a slot view. Only validation_flags drive
// the engine; presentation_flags are documentation/formatting metadata
// that do not affect call-time behaviour on method-arg slots.
static void slot_from_arg(typed_slot_t *out, const arg_decl_t *a) {
    out->name = a->name ? a->name : "";
    out->kind = a->kind;
    out->width = a->width;
    out->flags = a->validation_flags;
    out->enum_values = a->enum_values;
    out->default_value = a->default_value;
}

// Project an attribute member onto a slot view.
static void slot_from_attr(typed_slot_t *out, const member_t *m) {
    out->name = "";
    out->kind = m->attr.type;
    out->width = m->attr.width;
    out->flags = m->attr.validation_flags;
    out->enum_values = m->attr.enum_values;
    out->default_value = NULL;
}

// Human-readable kind name used in error messages.
static const char *kind_name(value_kind_t k) {
    switch (k) {
    case V_NONE:
        return "NONE";
    case V_BOOL:
        return "BOOL";
    case V_INT:
        return "INT";
    case V_UINT:
        return "UINT";
    case V_FLOAT:
        return "FLOAT";
    case V_STRING:
        return "STRING";
    case V_BYTES:
        return "BYTES";
    case V_ENUM:
        return "ENUM";
    case V_LIST:
        return "LIST";
    case V_OBJECT:
        return "OBJECT";
    case V_ERROR:
        return "ERROR";
    }
    return "?";
}

// True if the value's bit pattern fits in `width` bytes interpreted under
// the value's own signedness. `width=0` and `width=10` (FPU extended) mean
// "no explicit constraint" and we treat them as 8 bytes.
static bool value_fits_width(const value_t *v, uint8_t width) {
    if (width == 0 || width >= 8 || width == 10)
        return true;
    if (v->kind == V_INT) {
        int64_t lo = -((int64_t)1 << (8 * width - 1));
        int64_t hi = ((int64_t)1 << (8 * width - 1)) - 1;
        return v->i >= lo && v->i <= hi;
    }
    if (v->kind == V_UINT) {
        uint64_t cap = ((uint64_t)1 << (8 * width)) - 1;
        return v->u <= cap;
    }
    return true;
}

// Reinterpret bit pattern under the target signedness, width-bytes wide.
// Mutates *out in place; assumes value_fits_width has already passed.
static void coerce_int_sign(value_t *out, value_kind_t target_kind, uint8_t width) {
    uint8_t w = width ? width : 8;
    if (out->kind == V_INT && target_kind == V_UINT) {
        uint64_t mask = (w >= 8) ? UINT64_MAX : (((uint64_t)1 << (8 * w)) - 1);
        out->u = (uint64_t)out->i & mask;
        out->kind = V_UINT;
        out->width = w;
    } else if (out->kind == V_UINT && target_kind == V_INT) {
        if (w >= 8) {
            out->i = (int64_t)out->u;
        } else {
            uint64_t mask = ((uint64_t)1 << (8 * w)) - 1;
            uint64_t bits = out->u & mask;
            uint64_t sign = (uint64_t)1 << (8 * w - 1);
            if (bits & sign)
                out->i = (int64_t)(bits | ~mask);
            else
                out->i = (int64_t)bits;
        }
        out->kind = V_INT;
        out->width = w;
    }
}

// Format a "{a, b, c}" list of enum values for error messages.
static void format_enum_list(char *buf, size_t buf_size, const char *const *values) {
    size_t off = 0;
    int wrote = snprintf(buf + off, buf_size - off, "{");
    if (wrote > 0)
        off += (size_t)wrote;
    for (size_t i = 0; values && values[i] && off < buf_size; i++) {
        wrote = snprintf(buf + off, buf_size - off, "%s%s", i ? "," : "", values[i]);
        if (wrote > 0)
            off += (size_t)wrote;
    }
    snprintf(buf + off, buf_size - off, "}");
}

// Validate / coerce one slot. Writes a rewritten value to *out when coercion
// occurred (status VALIDATE_REWRITE). On failure, writes a one-line message
// to err_buf describing the constraint that failed (caller composes the
// final error string with method/attribute path prefix).
static validate_status_t validate_slot(const typed_slot_t *s, const value_t *in, value_t *out, char *err_buf,
                                       size_t err_size) {
    bool strict = (s->flags & OBJ_ARG_STRICT_KIND) != 0;
    bool rewrote = false;
    *out = *in;

    // V_NONE-kind slot is the "accept any kind" sentinel — body sees the
    // value as-is and does its own discrimination. Used today for slots
    // that legitimately accept multiple kinds (e.g. storage.hd_create's
    // size arg, which takes either a string label or an integer count).
    if (s->kind == V_NONE) {
        if ((s->flags & OBJ_ARG_NONEMPTY) && in->kind == V_STRING) {
            if (!in->s || !*in->s) {
                snprintf(err_buf, err_size, "must not be empty");
                return VALIDATE_ERR;
            }
        }
        return VALIDATE_OK;
    }

    if (in->kind != s->kind) {
        if (strict) {
            snprintf(err_buf, err_size, "must be %s, got %s", kind_name(s->kind), kind_name(in->kind));
            return VALIDATE_ERR;
        }
        // V_INT ↔ V_UINT with width fit + sign reinterpret.
        if ((s->kind == V_UINT && in->kind == V_INT) || (s->kind == V_INT && in->kind == V_UINT)) {
            if (s->width && !value_fits_width(in, s->width)) {
                if (in->kind == V_INT)
                    snprintf(err_buf, err_size, "= %" PRId64 " does not fit in %u bytes", in->i, s->width);
                else
                    snprintf(err_buf, err_size, "= 0x%" PRIx64 " does not fit in %u bytes", in->u, s->width);
                return VALIDATE_ERR;
            }
            coerce_int_sign(out, s->kind, s->width);
            rewrote = true;
        }
        // int → float
        else if (s->kind == V_FLOAT && (in->kind == V_INT || in->kind == V_UINT)) {
            bool ok = false;
            double f = val_as_f64(in, &ok);
            *out = (value_t){.kind = V_FLOAT, .width = 8, .f = f};
            rewrote = true;
        }
        // int 0/1 → bool
        else if (s->kind == V_BOOL && (in->kind == V_INT || in->kind == V_UINT)) {
            int64_t v = (in->kind == V_INT) ? in->i : (int64_t)in->u;
            if (v != 0 && v != 1) {
                snprintf(err_buf, err_size, "must be 0 or 1");
                return VALIDATE_ERR;
            }
            *out = (value_t){.kind = V_BOOL, .width = 1, .b = (v != 0)};
            rewrote = true;
        }
        // V_STRING → V_ENUM lookup
        else if (s->kind == V_ENUM && in->kind == V_STRING) {
            if (!s->enum_values) {
                snprintf(err_buf, err_size, "enum slot has no value table");
                return VALIDATE_ERR;
            }
            const char *str = in->s ? in->s : "";
            int idx = -1;
            size_t n_table = 0;
            while (s->enum_values[n_table])
                n_table++;
            for (size_t i = 0; i < n_table; i++) {
                if (strcmp(s->enum_values[i], str) == 0) {
                    idx = (int)i;
                    break;
                }
            }
            if (idx < 0) {
                char list_buf[120];
                format_enum_list(list_buf, sizeof(list_buf), s->enum_values);
                snprintf(err_buf, err_size, "must be one of %s, got '%.20s'", list_buf, str);
                return VALIDATE_ERR;
            }
            *out = (value_t){
                .kind = V_ENUM, .enm = {.idx = idx, .table = s->enum_values, .n_table = n_table}
            };
            rewrote = true;
        } else {
            snprintf(err_buf, err_size, "must be %s, got %s", kind_name(s->kind), kind_name(in->kind));
            return VALIDATE_ERR;
        }
    } else {
        // Kinds match — secondary checks.
        if (s->kind == V_ENUM && s->enum_values) {
            size_t n_table = 0;
            while (s->enum_values[n_table])
                n_table++;
            if (out->enm.idx < 0 || (size_t)out->enm.idx >= n_table) {
                snprintf(err_buf, err_size, "enum index %d out of range", out->enm.idx);
                return VALIDATE_ERR;
            }
        }
        if ((s->kind == V_INT || s->kind == V_UINT) && s->width) {
            if (!value_fits_width(out, s->width)) {
                if (out->kind == V_INT)
                    snprintf(err_buf, err_size, "= %" PRId64 " does not fit in %u bytes", out->i, s->width);
                else
                    snprintf(err_buf, err_size, "= 0x%" PRIx64 " does not fit in %u bytes", out->u, s->width);
                return VALIDATE_ERR;
            }
        }
        if (s->kind == V_OBJECT && !out->obj) {
            snprintf(err_buf, err_size, "must be a non-NULL object");
            return VALIDATE_ERR;
        }
    }

    // OBJ_ARG_NONEMPTY check on V_STRING.
    if ((s->flags & OBJ_ARG_NONEMPTY) && out->kind == V_STRING) {
        if (!out->s || !*out->s) {
            snprintf(err_buf, err_size, "must not be empty");
            return VALIDATE_ERR;
        }
    }

    return rewrote ? VALIDATE_REWRITE : VALIDATE_OK;
}

// Build a "<class>.<member>" prefix into buf. Used for the leading text
// in method/setter error messages.
static void format_member_path(char *buf, size_t buf_size, struct object *obj, const member_t *m) {
    const class_desc_t *cls = obj ? object_class(obj) : NULL;
    const char *cls_name = (cls && cls->name) ? cls->name : "";
    const char *member_name = (m && m->name) ? m->name : "";
    if (*cls_name && *member_name)
        snprintf(buf, buf_size, "%s.%s", cls_name, member_name);
    else if (*member_name)
        snprintf(buf, buf_size, "%s", member_name);
    else
        snprintf(buf, buf_size, "%s", cls_name);
}

// Validate argv against a method's declared args[]. On success the body is
// invoked with `*out_argv` (which may alias the caller's argv if no rewrite
// was needed, or point at scratch[] otherwise). Caller owns `scratch` (a
// stack array of capacity OBJ_VALIDATE_MAX_ARGS). The framework does not
// take ownership of any heap memory in the caller's argv.
static value_t node_validate_args(struct object *obj, const member_t *m, int in_argc, const value_t *in_argv,
                                  value_t *scratch, int *out_argc, const value_t **out_argv) {
    const arg_decl_t *args = m->method.args;
    int nargs = m->method.nargs;

    char prefix[128];
    format_member_path(prefix, sizeof(prefix), obj, m);

    // No declared args[] table → opt out of framework validation entirely
    // (the body owns argc / kind checking). This matches the legacy
    // contract for methods that haven't been migrated yet, and for
    // genuinely-variadic helpers like `echo` whose shape isn't expressible
    // in the current arg_decl_t vocabulary.
    if (!args) {
        *out_argc = in_argc;
        *out_argv = in_argv;
        return val_none();
    }
    if (nargs <= 0) {
        if (in_argc != 0)
            return val_err("%s: too many arguments (got %d, want 0)", prefix, in_argc);
        *out_argc = 0;
        *out_argv = NULL;
        return val_none();
    }

    if (nargs > OBJ_VALIDATE_MAX_ARGS)
        return val_err("%s: declared arg count %d exceeds limit %d", prefix, nargs, OBJ_VALIDATE_MAX_ARGS);

    // Locate the rest slot, if any (must be last per registration check).
    bool has_rest = (nargs > 0) && (args[nargs - 1].validation_flags & OBJ_ARG_REST) != 0;
    int fixed_n = has_rest ? (nargs - 1) : nargs;

    // Arity check.
    for (int i = 0; i < fixed_n; i++) {
        if (i >= in_argc && !(args[i].validation_flags & OBJ_ARG_OPTIONAL) && !args[i].default_value) {
            return val_err("%s: missing argument '%s'", prefix, args[i].name ? args[i].name : "?");
        }
    }
    if (!has_rest && in_argc > nargs) {
        return val_err("%s: too many arguments (got %d, want %d)", prefix, in_argc, nargs);
    }

    bool any_rewrite = false;
    int eff_n = fixed_n;
    if (in_argc > eff_n)
        eff_n = in_argc;
    if (eff_n > OBJ_VALIDATE_MAX_ARGS)
        eff_n = OBJ_VALIDATE_MAX_ARGS;

    // Fixed slots.
    for (int i = 0; i < fixed_n; i++) {
        typed_slot_t s;
        slot_from_arg(&s, &args[i]);

        if (i >= in_argc) {
            // Optional missing — fill with default if provided.
            if (args[i].default_value) {
                scratch[i] = *args[i].default_value;
                any_rewrite = true;
            } else {
                // Optional without default — body sees argc < nargs.
                eff_n = i;
                break;
            }
            continue;
        }

        char err[160];
        value_t out_v;
        validate_status_t st = validate_slot(&s, &in_argv[i], &out_v, err, sizeof(err));
        if (st == VALIDATE_ERR) {
            return val_err("%s: '%s' %s", prefix, s.name, err);
        }
        scratch[i] = (st == VALIDATE_REWRITE) ? out_v : in_argv[i];
        if (st == VALIDATE_REWRITE)
            any_rewrite = true;
    }

    // Rest slot: validate each tail item against the rest slot's kind/width/etc.
    int eff_rest_n = 0;
    if (has_rest) {
        const arg_decl_t *rest = &args[nargs - 1];
        typed_slot_t s;
        slot_from_arg(&s, rest);
        // V_NONE rest accepts any kind without coercion.
        bool accept_any = (rest->kind == V_NONE);
        for (int i = fixed_n; i < in_argc; i++) {
            char err[160];
            value_t out_v;
            if (accept_any) {
                scratch[i] = in_argv[i];
            } else {
                validate_status_t st = validate_slot(&s, &in_argv[i], &out_v, err, sizeof(err));
                if (st == VALIDATE_ERR) {
                    return val_err("%s: rest item %d ('%s') %s", prefix, i - fixed_n, rest->name ? rest->name : "?",
                                   err);
                }
                scratch[i] = (st == VALIDATE_REWRITE) ? out_v : in_argv[i];
                if (st == VALIDATE_REWRITE)
                    any_rewrite = true;
            }
            eff_rest_n++;
        }
        eff_n = fixed_n + eff_rest_n;
    }

    *out_argv = (any_rewrite || in_argc < fixed_n) ? scratch : in_argv;
    *out_argc = eff_n;
    return val_none();
}

// Validate the input value of a setter against the attribute's slot.
// Mutates *v in place: if a rewrite occurs (e.g. V_STRING→V_ENUM), the
// rewritten inline value replaces *v and any heap memory the original
// owned is freed.
static value_t node_validate_set(struct object *obj, const member_t *m, value_t *v) {
    typed_slot_t s;
    slot_from_attr(&s, m);

    char prefix[128];
    format_member_path(prefix, sizeof(prefix), obj, m);

    char err[160];
    value_t out_v;
    validate_status_t st = validate_slot(&s, v, &out_v, err, sizeof(err));
    if (st == VALIDATE_ERR) {
        return val_err("%s %s", prefix, err);
    }
    if (st == VALIDATE_REWRITE) {
        // Free any heap owned by the original before swapping in the
        // inline-only rewritten value.
        value_free(v);
        *v = out_v;
    }
    return val_none();
}

// === Node operations =========================================================

#ifndef NDEBUG
// Result-kind sanity check for getters / method results / setter returns.
// V_ERROR is always allowed (in-band error path).
static void assert_return_matches(const typed_slot_t *slot, const value_t *out, const char *site) {
    (void)site;
    if (out->kind == V_ERROR)
        return;
    // V_NONE-kind slot is the "no constraint" sentinel; no return-kind check.
    if (slot->kind == V_NONE)
        return;
    if (slot->kind != out->kind) {
        fprintf(stderr, "[object] %s: kind mismatch (declared %s, got %s)\n", site, kind_name(slot->kind),
                kind_name(out->kind));
        assert(out->kind == slot->kind && "return kind mismatch");
    }
    if ((slot->kind == V_INT || slot->kind == V_UINT) && slot->width) {
        if (!value_fits_width(out, slot->width)) {
            fprintf(stderr, "[object] %s: width mismatch (declared %u bytes)\n", site, slot->width);
            assert(value_fits_width(out, slot->width) && "return width mismatch");
        }
    }
    if (slot->kind == V_ENUM && slot->enum_values) {
        size_t n_table = 0;
        while (slot->enum_values[n_table])
            n_table++;
        assert(out->enm.idx >= 0 && (size_t)out->enm.idx < n_table && "return enum index out of table");
    }
}
#endif

value_t node_get(node_t n) {
    if (!node_valid(n))
        return val_err("invalid node");
    if (!n.member)
        return val_obj(n.obj); // points at the object itself
    switch (n.member->kind) {
    case M_ATTR: {
        if (!n.member->attr.get)
            return val_err("attribute '%s' has no getter", n.member->name);
        value_t v = n.member->attr.get(n.obj, n.member);
        // Propagate display flags from the slot's presentation_flags
        // (VAL_HEX/VAL_DEC/VAL_BIN/VAL_VOLATILE/VAL_SENSITIVE) onto the
        // value so formatters see the intent without consulting the
        // descriptor separately. Per-member VAL_RO stays on member.flags
        // and does not propagate (it controls writability, not display).
        v.flags |= n.member->attr.presentation_flags;
#ifndef NDEBUG
        typed_slot_t slot;
        slot_from_attr(&slot, n.member);
        assert_return_matches(&slot, &v, "attr.get");
#endif
        return v;
    }
    case M_METHOD:
        return val_err("'%s' is a method; use a call form", n.member->name);
    case M_CHILD: {
        if (n.member->child.indexed) {
            if (!n.member->child.get)
                return val_err("indexed child '%s' has no get callback", n.member->name);
            struct object *c = n.member->child.get(n.obj, n.index);
            if (!c) {
                // The user-facing path is typically `<parent>[<i>]`
                // (the resolver auto-routes a bare integer segment
                // into the first indexed-child member). Name the
                // parent object rather than the internal member name
                // so the diagnostic matches what the user typed.
                const char *parent = object_name(n.obj);
                return val_err("'%s[%d]' is empty", parent ? parent : n.member->name, n.index);
            }
            return val_obj(c);
        }
        struct object *c = NULL;
        if (n.member->child.lookup)
            c = n.member->child.lookup(n.obj, n.member->name);
        if (!c)
            c = find_attached_child(n.obj, n.member->name);
        if (!c)
            return val_err("named child '%s' not present", n.member->name);
        return val_obj(c);
    }
    }
    return val_err("unknown member kind");
}

value_t node_set(node_t n, value_t v) {
    if (!node_valid(n)) {
        value_free(&v);
        return val_err("invalid node");
    }
    if (!n.member || n.member->kind != M_ATTR) {
        value_free(&v);
        return val_err("'%s' is not a settable attribute", n.member ? n.member->name : "(object)");
    }
    if (n.member->flags & VAL_RO) {
        value_free(&v);
        return val_err("'%s' is read-only", n.member->name);
    }
    if (!n.member->attr.set) {
        value_free(&v);
        return val_err("attribute '%s' has no setter", n.member->name);
    }
    value_t err = node_validate_set(n.obj, n.member, &v);
    if (err.kind == V_ERROR) {
        value_free(&v);
        return err;
    }
    value_t out = n.member->attr.set(n.obj, n.member, v);
#ifndef NDEBUG
    assert((out.kind == V_NONE || out.kind == V_ERROR) && "setter returned a value other than V_NONE / V_ERROR");
#endif
    return out;
}

value_t node_call(node_t n, int argc, const value_t *argv) {
    if (!node_valid(n))
        return val_err("invalid node");
    if (!n.member || n.member->kind != M_METHOD)
        return val_err("'%s' is not a method", n.member ? n.member->name : "(object)");
    if (!n.member->method.fn)
        return val_err("method '%s' has no implementation", n.member->name);

    value_t scratch[OBJ_VALIDATE_MAX_ARGS];
    int eff_argc = argc;
    const value_t *eff_argv = argv;
    value_t err = node_validate_args(n.obj, n.member, argc, argv, scratch, &eff_argc, &eff_argv);
    if (err.kind == V_ERROR)
        return err;

    value_t out = n.member->method.fn(n.obj, n.member, eff_argc, eff_argv);
#ifndef NDEBUG
    value_kind_t want = n.member->method.result;
    if (want == V_NONE) {
        assert((out.kind == V_NONE || out.kind == V_ERROR) && "method declared result V_NONE but returned a value");
    } else {
        typed_slot_t result_slot = {.kind = want};
        assert_return_matches(&result_slot, &out, "method.fn");
    }
#endif
    return out;
}
