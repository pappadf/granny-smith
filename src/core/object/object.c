// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// object.c
// Object-model substrate. See object.h for the contract.

#include "object.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// === Object representation ==================================================
//
// Objects form a tree. Each object holds a pointer to its parent and the
// head of a singly-linked list of attached children, threaded via
// next_sibling. Attach is O(1) (push at head); detach is O(N) over the
// parent's child list, which is fine for the small fan-outs we expect
// (≤ tens). Indexed children are not stored here — they are produced on
// demand by the class's member descriptor callbacks.

struct object {
    const class_desc_t *cls;
    void *instance_data;
    const char *name;
    struct object *parent;
    struct object *first_child;
    struct object *next_sibling;
};

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
    if (o->parent)
        object_detach(o);
    free(o);
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
        // Duplicate-name check within the class.
        for (size_t j = 0; j < i; j++) {
            if (cls->members[j].name && strcmp(cls->members[j].name, m->name) == 0) {
                if (err_buf && err_size)
                    snprintf(err_buf, err_size, "class %s: duplicate member '%s'", cls->name, m->name);
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

    // If `n` currently points at a member (attribute or method), there
    // is no further path to descend. (Children-of-children are produced
    // by re-entering with a fresh object node.)
    if (n.member && n.member->kind != M_CHILD)
        return bad;

    // The "current object" we descend from is either n.obj directly, or
    // the indexed child this node references.
    struct object *here = n.obj;
    if (n.member && n.member->kind == M_CHILD && n.member->child.indexed) {
        if (!n.member->child.get)
            return bad;
        struct object *child = n.member->child.get(n.obj, n.index);
        if (!child)
            return bad;
        here = child;
    } else if (n.member && n.member->kind == M_CHILD && !n.member->child.indexed) {
        // Named child member — descend into its target.
        struct object *child = NULL;
        if (n.member->child.lookup)
            child = n.member->child.lookup(n.obj, n.member->name);
        if (!child)
            child = find_attached_child(n.obj, n.member->name);
        if (!child)
            return bad;
        here = child;
    }

    // Pure-integer segment → indexed child of the current here. We need
    // a CHILD member of here's class that is indexed; conventionally
    // there is at most one (the collection's own indexed member). For
    // M1 we take the first indexed child member found. (Real classes
    // typically have one such, e.g. scsi.devices.)
    {
        char *endp = NULL;
        long v = strtol(segment, &endp, 0);
        if (endp && endp != segment && *endp == '\0') {
            const class_desc_t *cls = object_class(here);
            if (!cls)
                return bad;
            for (size_t i = 0; i < cls->n_members; i++) {
                const member_t *m = &cls->members[i];
                if (m->kind == M_CHILD && m->child.indexed) {
                    return (node_t){.obj = here, .member = m, .index = (int)v};
                }
            }
            return bad;
        }
    }

    // Identifier segment → named member of here's class.
    const class_desc_t *cls = object_class(here);
    const member_t *m = class_find_member(cls, segment);
    if (m)
        return (node_t){.obj = here, .member = m, .index = -1};

    // Identifier may also name a statically-attached child object that
    // the class did not predeclare as a member (rare, but allowed for
    // root-style aggregations).
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

// === Node operations =========================================================

value_t node_get(node_t n) {
    if (!node_valid(n))
        return val_err("invalid node");
    if (!n.member)
        return val_obj(n.obj); // points at the object itself
    switch (n.member->kind) {
    case M_ATTR: {
        if (!n.member->attr.get)
            return val_err("attribute '%s' has no getter", n.member->name);
        value_t v = n.member->attr.get(n.obj);
        // Propagate display flags from the descriptor onto the value so
        // formatters see the intent ('VAL_HEX', etc.) without consulting
        // the descriptor separately.
        v.flags |= (uint16_t)n.member->flags;
        return v;
    }
    case M_METHOD:
        return val_err("'%s' is a method; use a call form", n.member->name);
    case M_CHILD: {
        if (n.member->child.indexed) {
            if (!n.member->child.get)
                return val_err("indexed child '%s' has no get callback", n.member->name);
            struct object *c = n.member->child.get(n.obj, n.index);
            if (!c)
                return val_err("'%s[%d]' is empty", n.member->name, n.index);
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
    return n.member->attr.set(n.obj, v);
}

value_t node_call(node_t n, int argc, const value_t *argv) {
    if (!node_valid(n))
        return val_err("invalid node");
    if (!n.member || n.member->kind != M_METHOD)
        return val_err("'%s' is not a method", n.member ? n.member->name : "(object)");
    if (!n.member->method.fn)
        return val_err("method '%s' has no implementation", n.member->name);
    return n.member->method.fn(n.obj, argc, argv);
}
