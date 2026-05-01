// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// object.h
// Object-model substrate: classes, members, objects, nodes, path resolution.
// See local/gs-docs/notes/proposal-module-object-model.md §2–§3.
//
// M1 lands the substrate only — no concrete classes are populated yet.
// Tests construct toy classes directly to exercise the resolver.
//
// Indexed-child stability: indices are sparse and stable. New entries
// receive max_index_ever + 1; removed indices are never recycled. The
// child descriptor's get/count/next callbacks implement that contract;
// this module does not assume a particular collection storage strategy.

#ifndef GS_OBJECT_OBJECT_H
#define GS_OBJECT_OBJECT_H

#include <stdbool.h>
#include <stddef.h>

#include "common.h"
#include "value.h"

#ifdef __cplusplus
extern "C" {
#endif

struct object;
struct member;
struct class_desc;

// === Method argument declaration =============================================

#define ARG_OPTIONAL 0x0001u // trailing optional argument
#define ARG_REST     0x0002u // slurp all remaining arguments into a V_LIST

// One declared parameter on a method. Mirrors proposal §3.
typedef struct arg_decl {
    const char *name;
    value_kind_t kind;
    unsigned flags; // ARG_OPTIONAL | ARG_REST | VAL_HEX | ...
    const char *const *enum_values; // NULL-terminated table for V_ENUM
    const char *doc;
} arg_decl_t;

// === Function pointer types ==================================================

typedef value_t (*attr_get_fn)(struct object *self);
typedef value_t (*attr_set_fn)(struct object *self, value_t in);
typedef value_t (*method_fn)(struct object *self, int argc, const value_t *argv);
typedef struct object *(*child_get_fn)(struct object *self, int index);
typedef int (*child_count_fn)(struct object *self);
typedef int (*child_next_fn)(struct object *self, int prev_index);
typedef struct object *(*child_lookup_fn)(struct object *self, const char *name);

// === Member descriptor =======================================================

typedef enum {
    M_ATTR = 1,
    M_METHOD,
    M_CHILD,
} member_kind_t;

// One member of a class. Member tables are static const; the framework
// iterates them for resolution, completion, and help. No string-search
// for attribute names happens at runtime beyond a linear scan of the
// (small) table.
typedef struct member {
    member_kind_t kind;
    const char *name;
    const char *doc;
    unsigned flags; // VAL_RO, VAL_VOLATILE, VAL_HEX, ...
    union {
        struct {
            value_kind_t type;
            attr_get_fn get;
            attr_set_fn set; // NULL → read-only
        } attr;
        struct {
            const arg_decl_t *args;
            int nargs;
            value_kind_t result;
            method_fn fn;
        } method;
        struct {
            const struct class_desc *cls;
            bool indexed;
            // Indexed children: get(i)→object|NULL (NULL = hole), count→live,
            //                   next(prev)→next live index or -1. Pass -1 to start.
            // Named children:   lookup(name)→object|NULL.
            child_get_fn get;
            child_count_fn count;
            child_next_fn next;
            child_lookup_fn lookup; // for named children (when not statically attached)
        } child;
    };
} member_t;

// === Class descriptor ========================================================

// Static description shared by all instances of a class.
typedef struct class_desc {
    const char *name;
    const member_t *members;
    size_t n_members;
    void *(*instance_data)(struct object *o); // optional, for casts
} class_desc_t;

// === Object lifetime =========================================================

// Construct an opaque object instance. instance_data is the back-pointer
// to module-private state (cpu_t*, scsi_t*, ...) that getters/setters
// cast through cls->instance_data. name is borrowed and must outlive the
// object. Returns NULL on allocation failure.
struct object *object_new(const class_desc_t *cls, void *instance_data, const char *name);

// Detach if attached, then free. Children of statically-described
// classes are not auto-deleted (they belong to the parent's storage).
void object_delete(struct object *o);

// Tree topology. Both object_attach and object_detach are O(1).
// object_attach asserts that child is not already attached elsewhere.
// Named statically-attached children are looked up by walking the
// attached list; indexed children are looked up via the child member's
// get/next callbacks instead.
void object_attach(struct object *parent, struct object *child);
void object_detach(struct object *child);

// Borrowed accessors.
const class_desc_t *object_class(const struct object *o);
const char *object_name(const struct object *o);
void *object_data(struct object *o);
struct object *object_parent(struct object *o);

// Iterate this object's statically-attached children (named children
// added via object_attach). Calls fn for each. Indexed children declared
// via member_t.child.get/next are not visited here.
void object_each_attached(struct object *o, void (*fn)(struct object *parent, struct object *child, void *ud),
                          void *ud);

// Linear lookup of a member by name. Returns NULL if not found.
const member_t *class_find_member(const class_desc_t *cls, const char *name);

// === Node and resolution =====================================================

// Resolution returns a node = (object, member, index). The node is the
// single entity the shell, JSON bridge, scripts, and held breakpoint
// references all carry.
typedef struct node {
    struct object *obj;
    const member_t *member; // NULL if the path resolves to an object itself
    int index; // for M_CHILD with indexed=true; -1 otherwise
} node_t;

// True if the node is bound to something resolvable.
static inline bool node_valid(node_t n) {
    return n.obj != NULL;
}

// Resolve a dotted path string against `root`. The path may include
// segments of the form `name`, `[index]`, or `.index`. Returns a node
// with obj=NULL on failure. M1 supports paths only — no method calls
// (those are the shell's job to assemble); the resolver returns the
// method's member in `member` so the caller can node_call it.
node_t object_resolve(struct object *root, const char *path);

// Read / write / call by node. node_get on an M_CHILD member returns
// V_OBJECT pointing at the child; on M_METHOD returns V_ERROR (use
// node_call). node_set on a read-only attribute returns V_ERROR.
value_t node_get(node_t n);
value_t node_set(node_t n, value_t v);
value_t node_call(node_t n, int argc, const value_t *argv);

// Single-segment descent. Used by the resolver and by the completer.
node_t node_child(node_t n, const char *segment);

// === Reserved-word check =====================================================

// Reserved words may not be used as member names, alias names, or any
// future user-bindable identifier. Members listed in §2.3 of the
// proposal: boolean-literal spellings + script-grammar keywords.
//
// Returns true if `name` collides with a reserved word.
bool object_is_reserved_word(const char *name);

// Validate a candidate member/alias name. Returns true if acceptable.
// Diagnostic messages are written to err_buf (may be NULL).
bool object_validate_name(const char *name, char *err_buf, size_t err_size);

// Verify class definition at registration time: every member name must
// be a valid identifier, must not collide with a reserved word, and
// must be unique within the class. Returns true on success; on failure
// writes a one-line message into err_buf (may be NULL).
bool object_validate_class(const class_desc_t *cls, char *err_buf, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_OBJECT_H
