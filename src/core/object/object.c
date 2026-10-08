// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// object.c
// Object-model substrate. See object.h for the contract.

#include "object.h"

#include "parse.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gs_assert.h"
#include "meta.h"
#include "job/job.h"

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
    const char *label; // optional display label; NULL = use name
    int order; // ordering weight for the SYSTEM tree; default 0
    int attach_seq; // monotonic attach sequence; the stable tiebreak for order
    uint16_t category; // M_CAT_* visibility for attached nodes
    uint8_t domain; // OBJ_DOMAIN_* for root children
    const char *doc; // optional one-sentence doc; NULL = the class's
    object_dtor_fn dtor; // optional destructor for instance_data (default NULL)
    struct object *parent;
    struct object *first_child;
    struct object *next_sibling;
    struct invalidator *invalidators; // weak-ref callbacks for held nodes
    struct object *meta_node; // lazily-created Meta node bound to this object (see meta.c)
    struct object *lparent; // logical (non-owning) parent of a callback-backed child; NULL = none
    char *lname; // owned: named-child segment or entry key (see lkeyed)
    int lindex; // entry index, or -1
    bool lkeyed; // true: lname is a collection key, not a member name
    int entry_index; // index its object_cache made it at, or -1
};

// Monotonic counter handed out at each object_attach, giving attached
// children a stable insertion order independent of the head-push storage.
// Used as the tiebreak in object_each_attached_ordered.
static int g_attach_seq = 0;

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
    while (g_root->first_child)
        object_detach(g_root->first_child);
    // Route through object_delete rather than freeing directly.
    //
    // This path used to detach, release the Meta node and free() -- skipping
    // object_fire_invalidators and the destructor hook, both of which
    // object_delete runs.  The invalidator contract is the mechanism that
    // makes a held node safe: shell_var.c's binding_store registers one on
    // whatever V_OBJECT it holds, and without the fire it keeps a `watched`
    // pointer into freed memory and is never marked stale, so the next read
    // dereferences it.  This was the one path that bypassed it.
    //
    // Blast radius is small -- tests and process exit are the callers -- but
    // it is the contract, not an optimisation, and a path that opts out of it
    // is how the next holder gets a dangling pointer.
    struct object *root = g_root;
    g_root = NULL; // clear first: the destructor must not re-enter through it
    object_delete(root);
}

#ifndef GS_FAST
// object_validate_class() is the object model's only structural check, and
// until now the only thing that ran it was root.c's stub registrar -- about a
// dozen classes.  The hundred-odd that reach the tree through object_new()
// were never checked at all.
//
// That gap is not hypothetical.  A block-scope `static const class_desc_t
// ppc_mmu_class;` in ppc.c is not a declaration of the file-scope descriptor,
// it is a second, zero-filled object that shadows it -- so machine.cpu.mmu and
// machine.cpu.fpu on every PowerPC machine came up with a NULL class name and
// no members, and nothing anywhere said a word.
//
// So validate here, where every node passes.  The result is cached per
// descriptor because validation is O(members^2) in the duplicate-name check
// and some classes (AppleTalk sessions, SCSI devices) are instantiated at
// runtime rather than once at boot.
//
// A failure prints and continues rather than refusing the object: the node
// staying absent is the quiet failure this is here to end.  Stripped entirely
// under GS_FAST -- the shipping build trusts what the debug build proved.
#define OBJ_VALIDATED_CACHE 192
static const class_desc_t *g_validated[OBJ_VALIDATED_CACHE];
static size_t g_validated_count;

static void validate_class_once(const class_desc_t *cls) {
    for (size_t i = 0; i < g_validated_count; i++)
        if (g_validated[i] == cls)
            return;
    char err[200];
    if (!object_validate_class(cls, err, sizeof(err)))
        fprintf(stderr, "object: class '%s' invalid: %s\n", cls->name ? cls->name : "(unnamed)", err);
    if (g_validated_count < OBJ_VALIDATED_CACHE)
        g_validated[g_validated_count++] = cls;
}
#endif

struct object *object_new(const class_desc_t *cls, void *instance_data, const char *name) {
    if (!cls)
        return NULL;
#ifndef GS_FAST
    validate_class_once(cls);
#endif
    struct object *o = (struct object *)calloc(1, sizeof(*o));
    if (!o)
        return NULL;
    o->cls = cls;
    o->instance_data = instance_data;
    o->name = name;
    o->lindex = -1;
    o->entry_index = -1;
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
    // Let the module release the C struct behind instance_data (and any
    // unattached collection-item wrappers it owns) before the substrate
    // frees the wrapper. Default NULL → no-op, so existing callers that
    // free their own structs are unaffected.
    if (o->dtor)
        o->dtor(o);
    if (o->parent)
        object_detach(o);
    // Drop the weak back-link so the parent's invalidator list does not keep
    // a callback into freed memory.
    object_set_logical_parent(o, NULL, NULL, -1, NULL);
    free(o);
}

void object_delete_tree(struct object *o) {
    if (!o)
        return;
    // Post-order: tear down every owned (attached) child first, depth-first.
    // Each recursive call detaches the child from `o`, so the loop drains
    // first_child until none remain. Reference edges and indexed-collection
    // items are callback-backed (never attached) and so are never visited.
    while (o->first_child)
        object_delete_tree(o->first_child);
    // Now `o` has no owned children; tear `o` itself down via the shared path
    // (fires invalidators, releases meta, runs the destructor, frees).
    object_delete(o);
}

void object_set_destructor(struct object *o, object_dtor_fn dtor) {
    if (o)
        o->dtor = dtor;
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

    // NOTE: two children of one parent sharing a name is always a bug, and a
    // silent one -- storage is head-push and find_attached_child() returns the
    // first match, so a duplicate shadows the original and every path through
    // it addresses the wrong device.  The Q900/Q950 carried two SCSI buses
    // both named "scsi" for a long time and nothing reported it.
    //
    // A GS_ASSERTF here would catch that class outright, and it cannot go in
    // yet: the checkpoint *restore* path constructs the new machine's objects
    // before tearing the outgoing machine's tree down, so every restore
    // transiently has two `memory`, `cpu`, `rtc`, `scsi`, ... under the same
    // parent.  It is benign today (head-push means lookups resolve to the new
    // object, and the stale generation is detached immediately after -- the
    // overlap never exceeds one generation), but it makes the invariant
    // un-assertable.  Ordering restore as destroy-then-create is the
    // prerequisite; the guard belongs with that change, not ahead of it.
    child->parent = parent;
    child->next_sibling = parent->first_child;
    parent->first_child = child;
    // Stamp a monotonic sequence so ordered iteration has a stable tiebreak
    // even though storage is head-push (LIFO).
    child->attach_seq = ++g_attach_seq;
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
value_t obj_u64_at(const void *block, const member_t *m) {
    if (!block)
        return val_uint(8, 0);
    size_t offset = (size_t)(uintptr_t)m->attr.user_data;
    uint64_t v;
    memcpy(&v, (const uint8_t *)block + offset, sizeof(v));
    return val_uint(8, v);
}

DEF_GETTER(obj_u64_field_get) {
    return obj_u64_at(object_data(self), m);
}

struct object *object_parent(struct object *o) {
    return o ? o->parent : NULL;
}

// Invalidator registered on a logical parent: the parent is going away, so
// the child forgets it (it stays alive; it just loses its path).
static void logical_parent_gone(void *ud) {
    struct object *child = (struct object *)ud;
    child->lparent = NULL;
    free(child->lname);
    child->lname = NULL;
    child->lindex = -1;
    child->lkeyed = false;
}

void object_set_logical_parent(struct object *obj, struct object *parent, const char *name, int index,
                               const char *key) {
    if (!obj)
        return;
    // Replace any previous link, including its registration on the old parent.
    if (obj->lparent)
        object_unregister_invalidator(obj->lparent, logical_parent_gone, obj);
    free(obj->lname);
    obj->lparent = NULL;
    obj->lname = NULL;
    obj->lindex = -1;
    obj->lkeyed = false;
    if (!parent || parent == obj)
        return;
    if (key) {
        GS_ASSERTF(object_valid_key(key), "logical parent key '%s' is not a short identifier", key);
        obj->lname = strdup(key);
        obj->lkeyed = true;
    } else if (name) {
        obj->lname = strdup(name);
    } else if (index < 0) {
        return; // nothing to name the segment with
    }
    if ((key || name) && !obj->lname)
        return; // out of memory: no link rather than a half one
    obj->lindex = (key || name) ? -1 : index;
    obj->lparent = parent;
    object_register_invalidator(parent, logical_parent_gone, obj);
}

struct object *object_logical_parent(struct object *o) {
    return o ? o->lparent : NULL;
}

const char *object_logical_name(struct object *o) {
    return (o && o->lparent && !o->lkeyed) ? o->lname : NULL;
}

int object_logical_index(struct object *o) {
    return (o && o->lparent && !o->lname) ? o->lindex : -1;
}

const char *object_logical_key(struct object *o) {
    return (o && o->lparent && o->lkeyed) ? o->lname : NULL;
}

bool object_valid_key(const char *key) {
    if (!key || !*key)
        return false;
    size_t n = 0;
    for (const char *p = key; *p; p++, n++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '_' || c == '.' || c == '-'))
            return false;
    }
    return n <= OBJ_KEY_MAX;
}

void object_each_attached(struct object *o, void (*fn)(struct object *parent, struct object *child, void *ud),
                          void *ud) {
    if (!o || !fn)
        return;
    for (struct object *c = o->first_child; c; c = c->next_sibling)
        fn(o, c, ud);
}

void object_set_label(struct object *o, const char *label) {
    if (o)
        o->label = label;
}

const char *object_label(struct object *o) {
    if (!o)
        return NULL;
    return o->label ? o->label : o->name;
}

void object_set_order(struct object *o, int order) {
    if (o)
        o->order = order;
}

int object_order(struct object *o) {
    return o ? o->order : 0;
}

void object_set_category(struct object *o, uint16_t category) {
    if (o)
        o->category = (uint16_t)(category & M_CAT_MASK);
}

uint16_t object_category(struct object *o) {
    return o ? o->category : (uint16_t)M_CAT_BASIC;
}

void object_set_doc(struct object *o, const char *doc) {
    if (o)
        o->doc = doc;
}

const char *object_doc(struct object *o) {
    if (!o)
        return "";
    if (o->doc)
        return o->doc;
    if (o->cls && o->cls->doc)
        return o->cls->doc;
    return "";
}

void object_set_domain(struct object *o, uint8_t domain) {
    if (o)
        o->domain = domain;
}

uint8_t object_domain(struct object *o) {
    return o ? o->domain : (uint8_t)OBJ_DOMAIN_EMULATOR;
}

const char *object_domain_name(uint8_t domain) {
    switch (domain) {
    case OBJ_DOMAIN_MACHINE:
        return "machine";
    case OBJ_DOMAIN_NETWORK:
        return "network";
    default:
        return "emulator";
    }
}

// Visit attached children in ascending (order, attach_seq). Fan-out is
// small (≤ tens), so a gather-then-insertion-sort into a fixed scratch
// array is fine; we fall back to raw order if the count exceeds the
// scratch capacity (no heap allocation on this path).
void object_each_attached_ordered(struct object *o, void (*fn)(struct object *parent, struct object *child, void *ud),
                                  void *ud) {
    if (!o || !fn)
        return;
    enum { MAX_SORTED = 128 };
    struct object *buf[MAX_SORTED];
    size_t n = 0;
    bool overflow = false;
    for (struct object *c = o->first_child; c; c = c->next_sibling) {
        if (n >= MAX_SORTED) {
            overflow = true;
            break;
        }
        buf[n++] = c;
    }
    if (overflow) {
        // Degenerate fan-out: keep it correct (visit all) even if unsorted.
        for (struct object *c = o->first_child; c; c = c->next_sibling)
            fn(o, c, ud);
        return;
    }
    // Stable insertion sort by (order, attach_seq).
    for (size_t i = 1; i < n; i++) {
        struct object *key = buf[i];
        size_t j = i;
        while (j > 0 && (buf[j - 1]->order > key->order ||
                         (buf[j - 1]->order == key->order && buf[j - 1]->attach_seq > key->attach_seq))) {
            buf[j] = buf[j - 1];
            j--;
        }
        buf[j] = key;
    }
    for (size_t i = 0; i < n; i++)
        fn(o, buf[i], ud);
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

bool arg_has_default(const arg_decl_t *a) {
    const value_t *d = a ? a->default_value : NULL;
    if (!d || d->kind == V_NONE)
        return false;
    return !(d->kind == V_STRING && (!d->s || !*d->s)); // "" means none given
}

void arg_doc_text(const arg_decl_t *a, char *buf, size_t size) {
    if (!buf || !size)
        return;
    const char *doc = (a && a->doc) ? a->doc : "";
    if (a && a->default_doc)
        snprintf(buf, size, "%s%somitted: %s", doc, *doc ? "; " : "", a->default_doc);
    else
        snprintf(buf, size, "%s", doc);
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
// are case-sensitive everywhere else in the codebase. This is the set
// docs/internals/core/object/object-model.md ("Reserved words") documents.

// Pure identifier: [A-Za-z_][A-Za-z0-9_]*
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

// A V_ENUM table must be NULL-terminated: validate_slot and the tab completer
// both walk one looking for the sentinel, so a table without it reads past its
// own end.  Checking only [0] -- which is all this used to do -- catches an
// absent table and misses an unterminated one, and three tables in the tree
// were unterminated.
//
// The bound is generous: it exists so a malformed table is a validation
// failure rather than a walk off the end, not to limit real enums.
#define OBJ_MAX_ENUM_VALUES 256

// Entries in a NULL-terminated enum table: the one scan validate_slot and the
// result check share. The descriptors carry no count, so it runs per call.
static size_t enum_table_len(const char *const *table) {
    size_t n = 0;
    while (table && table[n])
        n++;
    return n;
}

static bool enum_table_ok(const char *const *table) {
    if (!table || !table[0])
        return false;
    for (size_t i = 0; i < OBJ_MAX_ENUM_VALUES; i++)
        if (!table[i])
            return true;
    return false;
}

// Helper for the method-arg width checks.
static bool width_is_supported(uint8_t w) {
    // 0 = unconstrained, 1/2/4/8 = integer widths, 10 = FPU extended.
    return w == 0 || w == 1 || w == 2 || w == 4 || w == 8 || w == 10;
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
        // `meta` is reserved for the synthetic introspection node.
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

        // member_t.flags holds the visibility category and nothing else.
        if (m->flags & ~M_CAT_MASK) {
            if (err_buf && err_size)
                snprintf(err_buf, err_size, "%s.%s: flags 0x%x outside the category bits", cls->name, m->name,
                         (unsigned)m->flags);
            return false;
        }

        // Every attribute and method carries doc text.  The object tree is the
        // documentation -- `help machine.via1` is what a reader has instead of
        // a manual -- so a member without a `.doc` is a member that silently
        // documents nothing, and 76 of them had accumulated before anything
        // checked.  Debug builds only: the shipping build trusts the strings
        // the debug build proved are there, and does not carry the check.
        if ((m->kind == M_ATTR || m->kind == M_METHOD) && (!m->doc || !m->doc[0])) {
            if (err_buf && err_size)
                snprintf(err_buf, err_size, "%s.%s: %s has no .doc text", cls->name, m->name,
                         m->kind == M_ATTR ? "attribute" : "method");
            return false;
        }

        // A method with no args[] table opts out of validation entirely, so
        // it must not also claim an arity: a variadic method declares an
        // OBJ_ARG_REST slot instead.
        if (m->kind == M_METHOD && !m->method.args && m->method.nargs != 0) {
            if (err_buf && err_size)
                snprintf(err_buf, err_size, "%s.%s: nargs %d without an args[] table", cls->name, m->name,
                         m->method.nargs);
            return false;
        }

        // Method-arg ordering / coercion invariants.
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
                                 m->name, p->name ? p->name : "?", value_kind_name(p->default_value->kind),
                                 value_kind_name(p->kind));
                    return false;
                }
                if (p->default_value && p->default_doc) {
                    if (err_buf && err_size)
                        snprintf(err_buf, err_size, "%s.%s: arg '%s' has both a default and a default_doc", cls->name,
                                 m->name, p->name ? p->name : "?");
                    return false;
                }
                if (p->kind == V_ENUM && !enum_table_ok(p->enum_values)) {
                    if (err_buf && err_size)
                        snprintf(err_buf, err_size,
                                 "%s.%s: arg '%s' is V_ENUM but has a missing or unterminated enum_values table",
                                 cls->name, m->name, p->name ? p->name : "?");
                    return false;
                }
                if (!width_is_supported(p->width)) {
                    if (err_buf && err_size)
                        snprintf(err_buf, err_size, "%s.%s: arg '%s' has unsupported width %u (allowed: 0,1,2,4,8,10)",
                                 cls->name, m->name, p->name ? p->name : "?", p->width);
                    return false;
                }
            }
            if (rest_count > 1) {
                if (err_buf && err_size)
                    snprintf(err_buf, err_size, "%s.%s: only one rest arg allowed", cls->name, m->name);
                return false;
            }
        }

        // Collection invariants: one per class, and a way to reach its entries.
        if (member_is_collection(m)) {
            const collection_desc_t *c = m->child.collection;
            const char *why = NULL;
            if (class_collection(cls) != m)
                why = "a class has at most one collection member";
            else if (!c->by_index.get && !c->by_key.lookup)
                why = "a collection needs by_index.get or by_key.lookup";
            else if (c->by_index.next && !c->by_index.get)
                why = "by_index.next needs by_index.get";
            else if (c->by_key.next_key && !c->by_key.lookup)
                why = "by_key.next_key needs by_key.lookup";
            if (why) {
                if (err_buf && err_size)
                    snprintf(err_buf, err_size, "%s.%s: %s", cls->name, m->name, why);
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
            // V_ANY is a method-slot sentinel: an attribute needs a
            // concrete kind so the getter/setter round-trip and the
            // formatters have something to agree on.
            if (m->attr.type == V_ANY) {
                if (err_buf && err_size)
                    snprintf(err_buf, err_size, "%s.%s: V_ANY is not a valid attribute kind", cls->name, m->name);
                return false;
            }
            // A writable V_ENUM slot needs the table: node_set's V_STRING ->
            // V_ENUM coercion is the only thing that reads `enum_values` on an
            // attribute, and without a table it can only reject the write.
            // A read-only slot does not -- its getter builds the value with
            // val_enum(), carrying the table inside the value, which is how
            // image.type, scsi_bus.phase, scsi_device.type and floppy.type all
            // work.  Demanding a second copy in the descriptor would be asking
            // for two tables that can disagree.
            if (m->attr.type == V_ENUM && m->attr.set && !enum_table_ok(m->attr.enum_values)) {
                if (err_buf && err_size)
                    snprintf(err_buf, err_size,
                             "%s.%s: writable V_ENUM attribute has a missing or unterminated enum_values table",
                             cls->name, m->name);
                return false;
            }
            if (!width_is_supported(m->attr.width)) {
                if (err_buf && err_size)
                    snprintf(err_buf, err_size, "%s.%s: attribute has unsupported width %u (allowed: 0,1,2,4,8,10)",
                             cls->name, m->name, m->attr.width);
                return false;
            }
        }
    }
    return true;
}

void object_member_doc_gaps(const member_t *m, object_doc_gap_fn report, void *ud) {
    if (!m || !report || m->kind != M_METHOD)
        return;
    bool basic = member_is_basic(m);
    for (int k = 0; k < m->method.nargs && m->method.args; k++) {
        const arg_decl_t *a = &m->method.args[k];
        if (basic && (!a->doc || !*a->doc))
            report(m, a, "argument has no doc", ud);
        if ((a->kind == V_ANY || a->kind == V_NONE) && !(a->validation_flags & OBJ_ARG_POLY))
            report(m, a, "untyped argument (V_ANY/V_NONE) without OBJ_ARG_POLY", ud);
    }
    if (m->method.result == V_ANY && !m->method.result_doc)
        report(m, NULL, "V_ANY result without result_doc", ud);
}

// === Tree walk ===============================================================

// An open-addressed set of pointers, for the walk's visited objects and
// classes.
typedef struct {
    const void **slots;
    size_t cap, n;
} ptr_set_t;

// Hash a pointer onto a power-of-two table.
static size_t ptr_hash(const void *p, size_t cap) {
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 17;
    x *= (uintptr_t)0x9E3779B97F4A7C15ull;
    return (size_t)(x >> 7) & (cap - 1);
}

// Add `p`; false when it was already there (or the set cannot grow).
static bool ptr_set_add(ptr_set_t *s, const void *p) {
    if ((s->n + 1) * 2 > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 256;
        const void **t = (const void **)calloc(cap, sizeof(*t));
        if (!t)
            return false;
        for (size_t i = 0; i < s->cap; i++) {
            if (!s->slots[i])
                continue;
            size_t h = ptr_hash(s->slots[i], cap);
            while (t[h])
                h = (h + 1) & (cap - 1);
            t[h] = s->slots[i];
        }
        free(s->slots);
        s->slots = t;
        s->cap = cap;
    }
    size_t h = ptr_hash(p, s->cap);
    while (s->slots[h]) {
        if (s->slots[h] == p)
            return false;
        h = (h + 1) & (s->cap - 1);
    }
    s->slots[h] = p;
    s->n++;
    return true;
}

typedef struct {
    const object_visitor_t *v;
    void *ud;
    ptr_set_t objects, classes;
} walk_t;

static void walk_object(walk_t *w, struct object *o, const member_t *via, const char *fallback, bool basic);

// "<path>.<name>", or the name at the root; false when it does not fit.
static bool join_path(char *out, size_t size, const char *path, const char *name) {
    return snprintf(out, size, "%s%s%s", path, *path ? "." : "", name) < (int)size;
}

typedef struct {
    walk_t *w;
    const char *path;
    bool basic;
} walk_attached_t;

static void walk_attached(struct object *parent, struct object *child, void *ud) {
    (void)parent;
    walk_attached_t *a = (walk_attached_t *)ud;
    if (!object_name(child))
        return;
    char p[OBJ_PATH_MAX];
    if (!join_path(p, sizeof(p), a->path, object_name(child)))
        return;
    walk_object(a->w, child, NULL, p, a->basic && (object_category(child) & M_CAT_MASK) == M_CAT_BASIC);
}

// Visit `o` once, under its canonical path (else `fallback`, the path it was
// reached by), then its members, named children, entries and attached
// children.
static void walk_object(walk_t *w, struct object *o, const member_t *via, const char *fallback, bool basic) {
    if (!o || !ptr_set_add(&w->objects, o))
        return;
    const class_desc_t *cls = object_class(o);
    bool first = cls && ptr_set_add(&w->classes, cls);
    char path[OBJ_PATH_MAX];
    object_compute_path(o, path, sizeof(path));
    if (!path[0] && o != object_root() && snprintf(path, sizeof(path), "%s", fallback) >= (int)sizeof(path))
        return;
    if (w->v->object && !w->v->object(o, via, path, basic, first, w->ud))
        return;
    // Below an entry or a named child nothing counts as the basic tier.
    bool sub_basic = via ? false : basic;
    for (size_t i = 0; cls && i < cls->n_members; i++) {
        const member_t *m = &cls->members[i];
        if (!m->name)
            continue;
        char mpath[OBJ_PATH_MAX];
        if (!join_path(mpath, sizeof(mpath), path, m->name))
            continue;
        if (w->v->member)
            w->v->member(o, m, mpath, first, w->ud);
        if (m->kind != M_CHILD || m->child.reference)
            continue;
        if (!m->child.collection) {
            if (m->child.lookup)
                walk_object(w, object_named_child(o, m), m, mpath, sub_basic && (m->flags & M_CAT_MASK) == M_CAT_BASIC);
            continue;
        }
        char ep[OBJ_PATH_MAX];
        for (int k = object_child_next(o, m, -1); k >= 0; k = object_child_next(o, m, k)) {
            if (snprintf(ep, sizeof(ep), "%s[%d]", path, k) < (int)sizeof(ep))
                walk_object(w, object_entry_at(o, m, k), m, ep, false);
        }
        for (const char *k = object_child_next_key(o, m, NULL); k; k = object_child_next_key(o, m, k)) {
            if (snprintf(ep, sizeof(ep), "%s[\"%s\"]", path, k) < (int)sizeof(ep))
                walk_object(w, object_entry_by_key(o, m, k), m, ep, false);
        }
    }
    walk_attached_t a = {.w = w, .path = path, .basic = sub_basic};
    object_each_attached_ordered(o, walk_attached, &a);
}

void object_walk(struct object *start, const object_visitor_t *v, void *ud) {
    if (!start || !v)
        return;
    walk_t w = {.v = v, .ud = ud};
    walk_object(&w, start, NULL, "", true);
    free(w.objects.slots);
    free(w.classes.slots);
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

// Read an integer at *p through the one integer grammar the object model
// has, parse_integer_literal (parse.c).  On success writes the value into
// *out and returns the position after the digits; NULL if none was found.
//
// This was a third hand-rolled scanner accepting decimal, 0x and 0b -- while
// parse_integer_literal also accepts 0o, 0d, `$`, `_` separators and u/i
// suffixes, and node_child's accepted base-0 octal.  Three grammars, all
// reachable from path resolution.
static const char *parse_int(const char *p, long long *out) {
    if (!p)
        return NULL;
    const char *q = p;
    value_t v = parse_integer_literal(&q);
    if (val_is_error(&v)) {
        value_free(&v);
        return NULL;
    }
    bool ok = false;
    int64_t iv = val_as_i64(&v, &ok);
    value_free(&v);
    if (!ok)
        return NULL;
    *out = (long long)iv;
    return q;
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

const member_t *class_collection(const class_desc_t *cls) {
    for (size_t i = 0; cls && i < cls->n_members; i++)
        if (member_is_collection(&cls->members[i]))
            return &cls->members[i];
    return NULL;
}

int object_child_next(struct object *self, const member_t *m, int prev) {
    if (!member_is_collection(m))
        return -1;
    const collection_desc_t *c = m->child.collection;
    if (c->by_index.next)
        return c->by_index.next(self, prev);
    if (!c->by_index.get)
        return -1;
    for (int i = prev < 0 ? 0 : prev + 1; i < c->by_index.slots; i++)
        if (c->by_index.get(self, i))
            return i;
    return -1;
}

const char *object_child_next_key(struct object *self, const member_t *m, const char *prev) {
    if (!member_is_collection(m) || !m->child.collection->by_key.next_key)
        return NULL;
    return m->child.collection->by_key.next_key(self, prev);
}

uint32_t object_collection_count(struct object *self, const member_t *m) {
    if (!member_is_collection(m))
        return 0;
    uint32_t n = 0;
    if (m->child.collection->by_index.get) {
        for (int i = object_child_next(self, m, -1); i >= 0; i = object_child_next(self, m, i))
            n++;
    } else {
        for (const char *k = object_child_next_key(self, m, NULL); k; k = object_child_next_key(self, m, k))
            n++;
    }
    return n;
}

// `count` for a collection container with no count of its own: its live
// entries.
static DEF_GETTER(synth_count_get) {
    return val_uint(4, object_collection_count(self, class_collection(object_class(self))));
}

static const member_t k_synth_count = {
    .kind = M_ATTR,
    .name = "count",
    .doc = "Live entries in the collection",
    .attr = {.type = V_UINT, .width = 4, .get = synth_count_get}
};

// Safety net for object_set_logical_parent: a callback-backed child whose
// creator did not register its logical parent gets one the first time the
// resolver hands it out, so it still has a path.  Owning (attached) children
// and reference edges are left alone.
static void adopt_logical(struct object *child, const member_t *m, struct object *parent, const char *name, int index,
                          const char *key) {
    if (!child || child->parent || child->lparent || m->child.reference)
        return;
    if (key && !object_valid_key(key))
        return;
    object_set_logical_parent(child, parent, name, index, key);
}

struct object *object_entry_at(struct object *self, const member_t *m, int index) {
    if (!member_is_collection(m) || index < 0 || !m->child.collection->by_index.get)
        return NULL;
    struct object *o = m->child.collection->by_index.get(self, index);
    adopt_logical(o, m, self, NULL, index, NULL);
    return o;
}

struct object *object_entry_by_key(struct object *self, const member_t *m, const char *key) {
    if (!member_is_collection(m) || !key || !m->child.collection->by_key.lookup)
        return NULL;
    struct object *o = m->child.collection->by_key.lookup(self, key);
    adopt_logical(o, m, self, NULL, -1, key);
    return o;
}

struct object *object_named_child(struct object *self, const member_t *m) {
    if (!m || m->kind != M_CHILD || m->child.collection)
        return NULL;
    struct object *o = m->child.lookup ? m->child.lookup(self, m->name) : NULL;
    if (o)
        adopt_logical(o, m, self, m->name, -1, NULL);
    else
        o = find_attached_child(self, m->name);
    return o;
}

// === Collection containers ===================================================

// A container class built for a collection descriptor by
// object_collection_new: the entries member, then the descriptor's verbs.
// Built once per descriptor and kept for the process, like a static one.
typedef struct generated_class {
    const collection_desc_t *coll;
    class_desc_t cls;
    struct generated_class *next;
    member_t members[];
} generated_class_t;

static generated_class_t *g_generated;

static const class_desc_t *collection_class(const collection_desc_t *coll) {
    for (generated_class_t *g = g_generated; g; g = g->next)
        if (g->coll == coll)
            return &g->cls;
    size_t n = 1 + coll->n_verbs;
    generated_class_t *g = (generated_class_t *)calloc(1, sizeof(*g) + n * sizeof(member_t));
    if (!g)
        return NULL;
    g->members[0] = (member_t)OBJ_ENTRIES(coll, coll->entries_doc);
    if (coll->n_verbs)
        memcpy(&g->members[1], coll->verbs, coll->n_verbs * sizeof(member_t));
    g->cls = (class_desc_t){.name = coll->name, .members = g->members, .n_members = n, .doc = coll->doc};
    g->coll = coll;
    g->next = g_generated;
    g_generated = g;
    return &g->cls;
}

struct object *object_collection_new(const collection_desc_t *coll, void *data, const char *name) {
    const class_desc_t *cls = coll ? collection_class(coll) : NULL;
    return cls ? object_new(cls, data, name) : NULL;
}

// === Entry caches ============================================================

// One entry of an object_cache: made at `index`, or under `key`.
struct object_cache_slot {
    int index; // -1 for a keyed entry
    char *key; // owned; NULL for an indexed entry
    struct object *obj;
};

// The cache's container is being freed: its entries lose their path, not
// their life.
static void cache_parent_gone(void *ud) {
    ((object_cache_t *)ud)->parent = NULL;
}

// Point one entry's logical parent at the cache's container.
static void cache_link(const object_cache_t *c, const struct object_cache_slot *e) {
    if (e->key && !object_valid_key(e->key))
        return;
    object_set_logical_parent(e->obj, c->parent, NULL, e->key ? -1 : e->index, e->key);
}

// Make a new entry and record it.
static struct object *cache_add(object_cache_t *c, int index, const char *key, void *data) {
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 8;
        struct object_cache_slot *t =
            (struct object_cache_slot *)realloc(c->slots, (size_t)cap * sizeof(struct object_cache_slot));
        if (!t)
            return NULL;
        c->slots = t;
        c->cap = cap;
    }
    char *k = key ? strdup(key) : NULL;
    if (key && !k)
        return NULL;
    struct object *o = object_new(c->cls, data, c->name);
    if (!o) {
        free(k);
        return NULL;
    }
    o->entry_index = index;
    struct object_cache_slot *e = &c->slots[c->n++];
    *e = (struct object_cache_slot){.index = index, .key = k, .obj = o};
    if (c->parent)
        cache_link(c, e);
    return o;
}

struct object *object_cache_find(const object_cache_t *c, int index) {
    for (int i = 0; c && i < c->n; i++)
        if (!c->slots[i].key && c->slots[i].index == index)
            return c->slots[i].obj;
    return NULL;
}

struct object *object_cache_at(object_cache_t *c, int index, void *data) {
    if (!c || index < 0)
        return NULL;
    struct object *o = object_cache_find(c, index);
    return o ? o : cache_add(c, index, NULL, data);
}

struct object *object_cache_key(object_cache_t *c, const char *key, void *data) {
    if (!c || !key)
        return NULL;
    for (int i = 0; i < c->n; i++)
        if (c->slots[i].key && strcmp(c->slots[i].key, key) == 0)
            return c->slots[i].obj;
    return cache_add(c, -1, key, data);
}

void object_cache_set_parent(object_cache_t *c, struct object *parent) {
    if (!c || c->parent == parent)
        return;
    if (c->parent)
        object_unregister_invalidator(c->parent, cache_parent_gone, c);
    c->parent = parent;
    if (parent)
        object_register_invalidator(parent, cache_parent_gone, c);
    for (int i = 0; i < c->n; i++)
        cache_link(c, &c->slots[i]);
}

// Free entry i, moving the last one into its place.
static void cache_drop(object_cache_t *c, int i) {
    struct object_cache_slot e = c->slots[i];
    c->slots[i] = c->slots[--c->n];
    free(e.key);
    object_delete_tree(e.obj);
}

void object_cache_sweep(object_cache_t *c, bool (*live)(struct object *entry, void *ud), void *ud) {
    for (int i = 0; c && live && i < c->n;) {
        if (live(c->slots[i].obj, ud))
            i++;
        else
            cache_drop(c, i);
    }
}

void object_cache_clear(object_cache_t *c) {
    if (!c)
        return;
    while (c->n > 0)
        cache_drop(c, c->n - 1);
    free(c->slots);
    c->slots = NULL;
    c->cap = 0;
    object_cache_set_parent(c, NULL);
}

int object_entry_index(struct object *entry) {
    return entry ? entry->entry_index : -1;
}

// Single-segment descent shared by node_child (a textual segment) and
// node_child_index (an integer the caller already has). `segment` is the
// identifier for a name segment, NULL for an integer one (`ival`).
static node_t node_child_at(node_t n, const char *segment, long long ival);

node_t node_child(node_t n, const char *segment) {
    if (!segment || !*segment)
        return (node_t){0};

    // Probe for a pure-integer segment up front. Integer segments can
    // either *select an index* on a pending indexed-child member
    // (`devices[0]`, `devices.0`) or *enter* the first indexed-child
    // member of the current object (`bucket.0` shorthand for
    // `bucket.<first-indexed>.0`). They look the same syntactically.
    bool is_int = false;
    long long ival = 0;
    {
        // One integer grammar, parse_integer_literal's.  This used to be
        // strtoll(segment, &endp, 0) -- base 0, so a leading '0' meant OCTAL,
        // while the bracket form `devices[010]` went through parse_int and
        // read base 10.  So `devices.010` selected index 8 and
        // `devices[010]` selected index 10, for the same object, and nobody
        // writing a script could predict which grammar applied where.
        const char *q = segment;
        value_t iv = parse_integer_literal(&q);
        if (!val_is_error(&iv) && q && *q == '\0') {
            bool ok = false;
            long long v = (long long)val_as_i64(&iv, &ok);
            if (ok) {
                is_int = true;
                ival = v;
            }
        }
        value_free(&iv);
    }
    return node_child_at(n, is_int ? NULL : segment, ival);
}

node_t node_child_index(node_t n, int64_t index) {
    return node_child_at(n, NULL, (long long)index);
}

static node_t node_child_at(node_t n, const char *segment, long long ival) {
    node_t bad = (node_t){0};
    bool is_int = (segment == NULL);
    if (!n.obj)
        return bad;
    // An index has to fit the int the collection callbacks take.
    if (is_int && (ival < INT_MIN || ival > INT_MAX))
        return bad;

    // Attribute / method nodes are leaves — no further descent.
    if (n.member && n.member->kind != M_CHILD)
        return bad;

    // Case 1: `n` is sitting on an indexed-child member with no index
    // chosen yet. An integer segment supplies that index. This is what
    // makes both `bucket.devices[0]` and `bucket.devices.0` work.
    if (member_is_collection(n.member) && n.index < 0 && is_int)
        return (node_t){.obj = n.obj, .member = n.member, .index = (int)ival};

    // Determine the "current object" we descend from. If `n` already
    // points at a fully-resolved child slot or a named-child member,
    // descend into the target object first, then resolve the segment
    // against that target.
    struct object *here = n.obj;
    if (n.member && n.member->kind == M_CHILD) {
        if (n.member->child.collection && n.index < 0)
            return bad; // case 1 above already handled int segments
        here = n.member->child.collection ? object_entry_at(n.obj, n.member, n.index)
                                          : object_named_child(n.obj, n.member);
        if (!here)
            return bad;
    }

    // Synthetic `meta` segment.
    // Every object implicitly carries a `meta` attribute whose value is a
    // Meta node bound to it. The segment intercept lives here — after the
    // M_CHILD descent computes the real target object, before the regular
    // member lookup — so paths like `cpu.meta`, `floppy.drives.0.meta`,
    // and bare `meta` (root) all resolve uniformly.
    if (!is_int && strcmp(segment, "meta") == 0) {
        struct object *meta = meta_node_for(here);
        if (!meta)
            return bad;
        return (node_t){.obj = meta, .member = NULL, .index = -1};
    }

    // Integer segment now means "the collection member of `here`" (a class
    // declares at most one): `bucket.0` is `bucket.<entries>.0`.
    if (is_int) {
        const member_t *m = class_collection(object_class(here));
        return m ? (node_t){.obj = here, .member = m, .index = (int)ival} : bad;
    }

    // Identifier segment → named member of here's class.
    const class_desc_t *cls = object_class(here);
    const member_t *m = class_find_member(cls, segment);
    if (m)
        return (node_t){.obj = here, .member = m, .index = -1};
    if (strcmp(segment, "count") == 0 && class_collection(cls))
        return (node_t){.obj = here, .member = &k_synth_count, .index = -1};

    // Identifier may also name a statically-attached child object the
    // class did not predeclare as a member (the root uses this — its
    // children are added via object_attach at runtime).
    struct object *child = find_attached_child(here, segment);
    if (child)
        return (node_t){.obj = child, .member = NULL, .index = -1};

    return bad;
}

// Name-key descent into an indexed collection (`volumes["Shared"]`).  The
// integer form addresses a slot; this form asks the collection to map a stable
// name onto whichever slot currently holds it, so a script can name what it
// means instead of tracking allocation order.
node_t node_child_key(node_t n, const char *key) {
    node_t bad = (node_t){0};
    if (!n.obj || !key || !*key)
        return bad;
    // Sitting on the collection member itself, or on its container.
    const member_t *m = n.member;
    if (!m)
        m = class_collection(object_class(n.obj));
    else if (!member_is_collection(m) || n.index >= 0)
        return bad; // a resolved entry or named child is not itself a collection
    struct object *hit = object_entry_by_key(n.obj, m, key);
    return hit ? (node_t){.obj = hit, .member = NULL, .index = -1} : bad;
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
        // Bracket form: [INT] or ["NAME"]
        if (*p == '[') {
            p = skip_ws(p + 1);
            if (*p == '"') {
                // Name key — the collection's lookup callback resolves it.
                const char *k = p + 1;
                const char *e = strchr(k, '"');
                if (!e || *skip_ws(e + 1) != ']')
                    return bad;
                char key[128];
                size_t kn = (size_t)(e - k);
                if (kn >= sizeof(key))
                    return bad;
                memcpy(key, k, kn);
                key[kn] = '\0';
                cur = node_child_key(cur, key);
                if (!node_valid(cur))
                    return bad;
                p = skip_ws(skip_ws(e + 1) + 1);
                if (*p == '.')
                    p = skip_ws(p + 1);
                continue;
            }
            long long idx = 0;
            const char *q = parse_int(p, &idx);
            if (!q)
                return bad;
            q = skip_ws(q);
            if (*q != ']')
                return bad;
            cur = node_child_index(cur, idx);
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
            cur = node_child_index(cur, idx);
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

// === Path printer ============================================================
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

// === Argument / setter validation ===========================================
//
// Single engine drives both: arg_decl_t (one per method param) and
// member.attr (one per attribute) project onto the same `typed_slot_t`
// view, then validate_slot() enforces kind / width / non-empty / enum
// rules with limited coercion. See docs/internals/core/object/object-model.md
// ("Typed dispatch validation").

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
// Render an enum's accepted values as `{a,b,c}`, truncating with an ellipsis
// rather than running off the end of `buf`.
//
// This accumulated `off += wrote` where `wrote` is snprintf's WOULD-HAVE
// length.  Once the names exceeded the buffer, `off` grew past `buf_size`,
// the loop exited on `off < buf_size`, and the closing brace was written at
// `buf + off` -- past the end of the array -- with `buf_size - off`
// underflowing to a near-SIZE_MAX size_t.
//
// It stayed latent while the longest enum table in the tree totalled about 45
// characters against a 120-byte buffer.  Typed `log.set` arguments armed
// it: the category slot is an enum over all 62 log categories, so
// a mistyped category now formats a ~400-character list.  The first thing
// the typed method did on a bad name was overflow this.
static void format_enum_list(char *buf, size_t buf_size, const char *const *values) {
    if (!buf || buf_size == 0)
        return;
    size_t off = 0;
    bool truncated = false;

    // Reserve room for the closing "}" (and "..." when truncating).
    const size_t tail = 5; // "...}" plus the NUL
    if (buf_size <= tail) {
        buf[0] = '\0';
        return;
    }
    const size_t limit = buf_size - tail;

    buf[off++] = '{';
    for (size_t i = 0; values && values[i]; i++) {
        size_t sep = i ? 1u : 0u;
        size_t n = strlen(values[i]);
        if (off + sep + n >= limit) {
            truncated = true;
            break;
        }
        if (sep)
            buf[off++] = ',';
        memcpy(buf + off, values[i], n);
        off += n;
    }
    if (truncated) {
        memcpy(buf + off, "...", 3);
        off += 3;
    }
    buf[off++] = '}';
    buf[off] = '\0';
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

    // `none` is a legal "unset" for a slot that says so
    if ((s->flags & OBJ_ARG_NONE_OK) && in->kind == V_NONE)
        return VALIDATE_OK;

    // V_ANY — and V_NONE, its historical spelling on an argument slot —
    // is the "accept any kind" sentinel: the body sees the value as-is
    // and does its own discrimination. Used today for slots that
    // legitimately accept multiple kinds (e.g. files.hd_create's size
    // arg, which takes either a string label or an integer count).
    if (s->kind == V_NONE || s->kind == V_ANY) {
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
            snprintf(err_buf, err_size, "must be %s, got %s", value_kind_name(s->kind), value_kind_name(in->kind));
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
        // string → bool: accept the classic switch spellings. `on`/`off`/
        // `yes`/`no` stopped being reserved words, so they
        // arrive as V_STRING from argument mode; map them here so bool slots
        // keep their old ergonomics.
        else if (s->kind == V_BOOL && in->kind == V_STRING) {
            const char *str = in->s ? in->s : "";
            bool bv;
            if (!val_parse_bool(str, &bv)) {
                snprintf(err_buf, err_size, "must be a boolean (true/false/on/off/yes/no), got '%.20s'", str);
                return VALIDATE_ERR;
            }
            *out = (value_t){.kind = V_BOOL, .width = 1, .b = bv};
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
            size_t n_table = enum_table_len(s->enum_values);
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
            snprintf(err_buf, err_size, "must be %s, got %s", value_kind_name(s->kind), value_kind_name(in->kind));
            return VALIDATE_ERR;
        }
    } else {
        // Kinds match — secondary checks.
        if (s->kind == V_ENUM && s->enum_values) {
            size_t n_table = enum_table_len(s->enum_values);
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

// Build the "<path>.<member>" prefix of a method/setter error message,
// naming the member the way the user addresses it (`machine.cpu.step`), with
// the same path printer meta.path uses. A node with no path (the root, or
// one not attached to the tree) falls back to its class name.
static void format_member_path(char *buf, size_t buf_size, struct object *obj, const member_t *m) {
    char path[OBJ_PATH_MAX];
    object_compute_path(obj, path, sizeof(path));
    const class_desc_t *cls = obj ? object_class(obj) : NULL;
    const char *head = *path ? path : (cls && cls->name) ? cls->name : "";
    const char *member_name = (m && m->name) ? m->name : "";
    if (*head && *member_name)
        snprintf(buf, buf_size, "%s.%s", head, member_name);
    else if (*member_name)
        snprintf(buf, buf_size, "%s", member_name);
    else
        snprintf(buf, buf_size, "%s", head);
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

    char prefix[256];
    format_member_path(prefix, sizeof(prefix), obj, m);

    // No declared args[] table → opt out of framework validation entirely
    // (the body owns argc / kind checking). object_validate_class only
    // allows this with nargs == 0; a variadic method declares an
    // OBJ_ARG_REST slot instead (root `echo` does).
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
    // Every given argument lands in scratch[] (a rest tail included), so the
    // count is bounded by its capacity -- refused, never clamped or overrun.
    if (in_argc > OBJ_VALIDATE_MAX_ARGS)
        return val_err("%s: too many arguments (got %d, limit %d)", prefix, in_argc, OBJ_VALIDATE_MAX_ARGS);

    // Locate the rest slot, if any (must be last per registration check).
    bool has_rest = (nargs > 0) && (args[nargs - 1].validation_flags & OBJ_ARG_REST) != 0;
    int fixed_n = has_rest ? (nargs - 1) : nargs;

    // Highest slot carrying a real value. A V_NONE in a fixed slot is the
    // named-arg binder's "unfilled" marker (also produced by JSON null) and
    // is treated exactly like a missing trailing argument.
    int last_given = -1;
    for (int i = 0; i < in_argc; i++) {
        if (i >= fixed_n || in_argv[i].kind != V_NONE)
            last_given = i;
    }

    // Arity check.
    for (int i = 0; i < fixed_n; i++) {
        bool missing = (i >= in_argc) || (in_argv[i].kind == V_NONE);
        if (missing && !(args[i].validation_flags & OBJ_ARG_OPTIONAL) && !args[i].default_value) {
            return val_err("%s: missing argument '%s'", prefix, args[i].name ? args[i].name : "?");
        }
    }
    if (!has_rest && in_argc > nargs) {
        return val_err("%s: too many arguments (got %d, want %d)", prefix, in_argc, nargs);
    }

    bool any_rewrite = false;
    // Every fixed slot is materialised in scratch -- given, default-filled, or
    // V_NONE for an optional the caller skipped -- so a body may read any
    // declared slot.  argc counts through the last slot that was given or
    // default-filled; an optional left out at the tail shortens it.
    int eff_n = 0;

    // Fixed slots.
    for (int i = 0; i < fixed_n; i++) {
        typed_slot_t s;
        slot_from_arg(&s, &args[i]);

        if (i >= in_argc || in_argv[i].kind == V_NONE) {
            // Optional missing (or a V_NONE hole) — fill with the default,
            // else with none.
            if (args[i].default_value) {
                scratch[i] = *args[i].default_value;
                eff_n = i + 1;
            } else if ((args[i].validation_flags & OBJ_ARG_GROUPED) && i < last_given) {
                // A grouped slot cannot be skipped alone.
                return val_err("%s: missing argument '%s'", prefix, args[i].name ? args[i].name : "?");
            } else {
                scratch[i] = val_none();
            }
            any_rewrite = true;
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
        eff_n = i + 1;
    }

    // Rest slot: validate each tail item against the rest slot's kind/width/etc.
    int eff_rest_n = 0;
    if (has_rest) {
        const arg_decl_t *rest = &args[nargs - 1];
        typed_slot_t s;
        slot_from_arg(&s, rest);
        // A V_ANY / V_NONE rest slot accepts any kind without coercion.
        bool accept_any = (rest->kind == V_NONE || rest->kind == V_ANY);
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

    *out_argv = any_rewrite ? scratch : in_argv;
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

    char prefix[256];
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
    (void)site; // only the GS_ASSERTF messages read it; GS_FAST compiles them out
    if (out->kind == V_ERROR)
        return;
    // V_ANY declares a polymorphic result; V_NONE reaches here only from
    // an argument-shaped slot, where it is the same "any kind" sentinel
    // (node_call handles a V_NONE *result* slot itself — there it means
    // "returns nothing"). Either way there is nothing to check.
    if (slot->kind == V_ANY || slot->kind == V_NONE)
        return;
    // An unset OBJ_ARG_NONE_OK slot answers `none`
    if ((slot->flags & OBJ_ARG_NONE_OK) && out->kind == V_NONE)
        return;
    // Through the project's assertion handler, which prints the message
    // with its diagnostics, rather than a bare stderr line before assert().
    GS_ASSERTF(out->kind == slot->kind, "%s: kind mismatch (declared %s, got %s)", site, value_kind_name(slot->kind),
               value_kind_name(out->kind));
    if ((slot->kind == V_INT || slot->kind == V_UINT) && slot->width)
        GS_ASSERTF(value_fits_width(out, slot->width), "%s: width mismatch (declared %u bytes)", site, slot->width);
    if (slot->kind == V_ENUM && slot->enum_values && out->kind == V_ENUM)
        GS_ASSERTF(out->enm.idx >= 0 && (size_t)out->enm.idx < enum_table_len(slot->enum_values),
                   "%s: enum index %d out of table", site, out->enm.idx);
}
#endif

static value_t node_get_here(node_t n) {
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
        // descriptor separately.
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
        if (n.member->child.collection) {
            if (!n.member->child.collection->by_index.get)
                return val_err("indexed child '%s' has no get callback", n.member->name);
            if (n.index < 0) {
                // Index-less read of an indexed collection returns the
                // whole collection as V_LIST of entry objects (`.entries`
                // is the data; the REPL's table formatter is the
                // presentation).
                value_t *items = NULL;
                size_t len = 0, cap = 0;
                for (int i = object_child_next(n.obj, n.member, -1); i >= 0;
                     i = object_child_next(n.obj, n.member, i)) {
                    struct object *c = object_entry_at(n.obj, n.member, i);
                    if (c && !val_list_push(&items, &len, &cap, val_obj(c))) {
                        free(items);
                        return val_err("out of memory");
                    }
                }
                return val_list(items, len);
            }
            struct object *c = object_entry_at(n.obj, n.member, n.index);
            if (!c) {
                // The user-facing path is typically `<parent>[<i>]`
                // (the resolver auto-routes a bare integer segment
                // into the collection member). Name the
                // parent object rather than the internal member name
                // so the diagnostic matches what the user typed.
                const char *parent = object_name(n.obj);
                return val_err("'%s[%d]' is empty", parent ? parent : n.member->name, n.index);
            }
            return val_obj(c);
        }
        struct object *c = object_named_child(n.obj, n.member);
        if (!c)
            return val_err("named child '%s' not present", n.member->name);
        return val_obj(c);
    }
    }
    return val_err("unknown member kind");
}

static value_t node_set_here(node_t n, value_t v) {
    if (!node_valid(n)) {
        value_free(&v);
        return val_err("invalid node");
    }
    if (!n.member || n.member->kind != M_ATTR) {
        value_free(&v);
        return val_err("'%s' is not a settable attribute", n.member ? n.member->name : "(object)");
    }
    if (!n.member->attr.set) {
        value_free(&v);
        return val_err("'%s' is read-only", n.member->name);
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

static value_t node_call_here(node_t n, int argc, const value_t *argv) {
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
    if (want == V_ANY) {
        // Polymorphic result: the kind is the method's own business
        // (debug.mac.globals.read hands back a uint or bytes depending on
        // the width of the named global). Nothing to assert.
    } else if (want == V_NONE) {
        assert((out.kind == V_NONE || out.kind == V_ERROR) && "method declared result V_NONE but returned a value");
    } else {
        typed_slot_t result_slot = {.kind = want};
        assert_return_matches(&result_slot, &out, "method.fn");
    }
#endif
    return out;
}

// === The seam ================================================================
//
// node_get / node_set / node_call are the only way to reach guest state,
// and guest state belongs to the emulator thread.  A caller on any other
// thread -- the interpreter on the job thread -- is handed over through
// job_on_emulator (job/job.h) and waits; on the emulator thread the
// call is direct.  Path resolution (object_resolve) walks the tree's
// structure only and stays where the caller is.

typedef struct {
    node_t n;
    int argc;
    const value_t *argv;
    value_t in;
    value_t out;
} node_marshal_t;

static void marshal_get(void *p) {
    node_marshal_t *m = (node_marshal_t *)p;
    m->out = node_get_here(m->n);
}

static void marshal_set(void *p) {
    node_marshal_t *m = (node_marshal_t *)p;
    m->out = node_set_here(m->n, m->in);
}

static void marshal_call(void *p) {
    node_marshal_t *m = (node_marshal_t *)p;
    m->out = node_call_here(m->n, m->argc, m->argv);
}

value_t node_get(node_t n) {
    if (job_on_emulator_thread())
        return node_get_here(n);
    node_marshal_t m = {.n = n};
    job_on_emulator(marshal_get, &m);
    return m.out;
}

// Inline mode (job.h): a tree call on the emulator thread that opens a mode
// is followed by the wait for it.  Weak: linked only where job.c is.
uint32_t job_glue_mode_id(void) __attribute__((weak));
void job_inline_after_call(uint32_t mode_before) __attribute__((weak));
bool job_inline_enabled(void) __attribute__((weak));

// The mode id before the call, tagged (top bit) when inline mode is on --
// 0 is a legitimate id, so the tag says whether to wait at all.
static inline uint32_t inline_mode_before(void) {
    if (job_inline_enabled && job_inline_enabled() && job_glue_mode_id && job_inline_after_call)
        return job_glue_mode_id() | 0x80000000u;
    return 0;
}

static inline void inline_mode_after(uint32_t before) {
    if (before & 0x80000000u)
        job_inline_after_call(before & 0x7fffffffu);
}

value_t node_set(node_t n, value_t v) {
    if (job_on_emulator_thread()) {
        uint32_t before = inline_mode_before();
        value_t r = node_set_here(n, v);
        inline_mode_after(before);
        return r;
    }
    node_marshal_t m = {.n = n, .in = v};
    job_on_emulator(marshal_set, &m);
    return m.out;
}

value_t node_call(node_t n, int argc, const value_t *argv) {
    if (job_on_emulator_thread()) {
        uint32_t before = inline_mode_before();
        value_t r = node_call_here(n, argc, argv);
        inline_mode_after(before);
        return r;
    }
    node_marshal_t m = {.n = n, .argc = argc, .argv = argv};
    job_on_emulator(marshal_call, &m);
    // A leaf that answered later (an I/O job) and failed: the failure is
    // the call's result, not the provisional value it returned at once.
    char err[256];
    if (job_call_take_failure(err, sizeof err)) {
        value_free(&m.out);
        return val_err("%s", err);
    }
    return m.out;
}

// Append the method's declared fixed-argument names to buf as a
// comma-separated list (for unknown-name error messages).
static void format_declared_names(char *buf, size_t buf_size, const arg_decl_t *args, int fixed_n) {
    size_t pos = 0;
    buf[0] = '\0';
    for (int i = 0; i < fixed_n; i++) {
        const char *name = args[i].name ? args[i].name : "?";
        int wrote = snprintf(buf + pos, buf_size - pos, "%s%s", i ? ", " : "", name);
        if (wrote < 0 || (size_t)wrote >= buf_size - pos)
            break;
        pos += (size_t)wrote;
    }
}

value_t node_bind_args(node_t n, int pos_argc, const value_t *pos_argv, int named_n, const named_arg_t *named,
                       value_t *out_argv, int *out_argc) {
    if (!node_valid(n) || !n.member || n.member->kind != M_METHOD)
        return val_err("named arguments: not a method");

    const arg_decl_t *args = n.member->method.args;
    int nargs = n.member->method.nargs;

    char prefix[256];
    format_member_path(prefix, sizeof(prefix), n.obj, n.member);

    // Methods without a declared args[] table don't participate in named
    // binding — positional-only.
    if (!args || nargs <= 0) {
        if (named_n > 0)
            return val_err("%s: method does not declare named arguments", prefix);
        if (pos_argc > OBJ_BIND_MAX_ARGS)
            return val_err("%s: too many arguments (got %d, limit %d)", prefix, pos_argc, OBJ_BIND_MAX_ARGS);
        for (int i = 0; i < pos_argc; i++)
            out_argv[i] = pos_argv[i];
        *out_argc = pos_argc;
        return val_none();
    }

    bool has_rest = (nargs > 0) && (args[nargs - 1].validation_flags & OBJ_ARG_REST) != 0;
    int fixed_n = has_rest ? (nargs - 1) : nargs;

    if (pos_argc > OBJ_BIND_MAX_ARGS || nargs > OBJ_BIND_MAX_ARGS)
        return val_err("%s: too many arguments (limit %d)", prefix, OBJ_BIND_MAX_ARGS);

    // Positionals fill slots left to right (a positional tail beyond
    // fixed_n feeds the rest slot exactly as before).
    int out_n = pos_argc;
    for (int i = 0; i < pos_argc; i++)
        out_argv[i] = pos_argv[i];

    bool named_filled[OBJ_BIND_MAX_ARGS] = {false};
    for (int k = 0; k < named_n; k++) {
        const char *name = named[k].name ? named[k].name : "";
        int idx = -1;
        for (int i = 0; i < fixed_n; i++) {
            if (args[i].name && strcmp(args[i].name, name) == 0) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            // The rest slot is positional-tail only — name it explicitly.
            if (has_rest && args[fixed_n].name && strcmp(args[fixed_n].name, name) == 0)
                return val_err("%s: argument '%s' is a rest slot and cannot be passed by name", prefix, name);
            char names[160];
            format_declared_names(names, sizeof(names), args, fixed_n);
            return val_err("%s: unknown argument '%s' (declared: %s)", prefix, name, names);
        }
        if (idx < pos_argc)
            return val_err("%s: duplicate argument '%s' (already given positionally)", prefix, name);
        if (named_filled[idx])
            return val_err("%s: duplicate argument '%s'", prefix, name);
        named_filled[idx] = true;
        out_argv[idx] = named[k].value;
        if (idx + 1 > out_n)
            out_n = idx + 1;
    }

    // Slots between the positional prefix and the highest named slot that
    // no named arg claimed become V_NONE holes for the validator.
    for (int i = pos_argc; i < out_n; i++) {
        if (!named_filled[i])
            out_argv[i] = val_none();
    }

    *out_argc = out_n;
    return val_none();
}
