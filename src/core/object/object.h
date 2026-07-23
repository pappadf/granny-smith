// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// object.h
// Object-model substrate: classes, members, objects, nodes, path resolution.
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

// Prefixed `OBJ_ARG_*` for a clear namespace on the typed object-model
// argument declarations.
#define OBJ_ARG_OPTIONAL    0x0001u // trailing optional argument
#define OBJ_ARG_REST        0x0002u // slurp all remaining arguments into a V_LIST
#define OBJ_ARG_NONEMPTY    0x0004u // V_STRING value must be non-NULL and non-empty
#define OBJ_ARG_STRICT_KIND 0x0008u // disable int↔uint, int→float, string→enum coercion
// OBJ_ARG_TEMPLATE — deferred-eval string slot (shell v2 §6.3): the
// command parser stores the raw, uninterpolated string body; the owning
// subsystem evaluates it later (possibly repeatedly) with extra
// bindings, e.g. logpoint messages with `$value`/`$addr`/`$size`.
#define OBJ_ARG_TEMPLATE 0x0020u

// === Member visibility category (proposal-system-object-model.md §7.2) =======
//
// A three-tier visibility classification, stored in the high bits of
// member_t.flags (the low bits hold VAL_RO/VAL_HEX/… from value.h, which
// occupy 0x0001..0x0020). Every consumer (SYSTEM tab, command browser)
// reads the category off the model so visibility cannot drift the way a
// hand-maintained allowlist did:
//   basic    — always shown in the tree; the default (0).
//   advanced — shown only with the "Advanced" toggle on.
//   internal — never shown in the tree, but fully scriptable.
#define M_CAT_BASIC    0x0000u // default — always shown
#define M_CAT_ADVANCED 0x0100u // shown only under the Advanced toggle
#define M_CAT_INTERNAL 0x0200u // never shown in UI; still scriptable
#define M_CAT_MASK     0x0300u // mask to extract the category bits

// === Method UI metadata (proposal-system-object-model.md §7.3) ===============
//
// Flags on a method member that steer context menus and the command
// browser. They hang off the existing method descriptor; no new subsystem.
#define MM_DESTRUCTIVE 0x0001u // confirm before invoking (eject, rm, …)
#define MM_MUTATE      0x0002u // changes state (vs a pure query)
#define MM_HIDDEN      0x0004u // not surfaced in UI menus / browser

// One declared parameter on a method. Mirrors proposal §3.
//
// Two flag fields (proposal §3.3):
//   `validation_flags` change what the validator does (OBJ_ARG_*).
//   `presentation_flags` steer formatters and inspectors (VAL_HEX,
//      VAL_DEC, VAL_BIN, VAL_VOLATILE, VAL_SENSITIVE) — no effect at
//      call time on a method-arg slot, but propagated to help text.
typedef struct arg_decl {
    const char *name;
    value_kind_t kind;
    uint8_t width; // 1/2/4/8 for V_INT/V_UINT range check; 0 = unconstrained
    uint16_t validation_flags; // OBJ_ARG_OPTIONAL | OBJ_ARG_REST | OBJ_ARG_NONEMPTY | OBJ_ARG_STRICT_KIND
    uint16_t presentation_flags; // VAL_HEX | VAL_DEC | VAL_BIN | ...
    const char *const *enum_values; // NULL-terminated table for V_ENUM
    const value_t *default_value; // optional default for OBJ_ARG_OPTIONAL slots
    const char *doc;
} arg_decl_t;

// === Function pointer types ==================================================
//
// Attribute getters/setters and method callbacks all receive their own
// member descriptor so dispatchers shared across many members (e.g. the
// 471-entry `mac` class with one getter for every global) can recover
// per-member context from `m->user_data`.

struct member;
typedef value_t (*attr_get_fn)(struct object *self, const struct member *m);
typedef value_t (*attr_set_fn)(struct object *self, const struct member *m, value_t in);
typedef value_t (*method_fn)(struct object *self, const struct member *m, int argc, const value_t *argv);
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
    uint16_t flags; // VAL_RO + M_CAT_* visibility (per-member, not per-slot)
    // Optional display label (proposal §7.1). The path segment stays
    // `name`; the tree shows `label` when present, else `name`. NULL = use
    // the name.
    const char *label;
    // Ordering weight for a faithful, deterministic tree (proposal §7.4).
    // Lower sorts earlier; ties break on declaration order. Default 0.
    int16_t order;
    union {
        struct {
            value_kind_t type;
            uint8_t width; // 1/2/4/8 for V_INT/V_UINT range check; 0 = unconstrained
            uint16_t validation_flags; // OBJ_ARG_NONEMPTY | OBJ_ARG_STRICT_KIND
            uint16_t presentation_flags; // VAL_HEX | VAL_DEC | VAL_BIN | VAL_VOLATILE | VAL_SENSITIVE
            const char *const *enum_values; // NULL-terminated table for V_ENUM
            attr_get_fn get;
            attr_set_fn set; // NULL → read-only
            const void *user_data; // borrowed; passed back via the `m` arg
        } attr;
        struct {
            const arg_decl_t *args;
            int nargs;
            value_kind_t result;
            method_fn fn;
            // UI metadata (proposal §7.3): MM_DESTRUCTIVE | MM_MUTATE | MM_HIDDEN.
            uint16_t ui_flags;
            // Short verb shown in menus ("Save image…") when distinct from
            // the method name ("export"). NULL = use the method name.
            const char *verb_label;
            // By-task grouping for the command browser ("storage", "debugger",
            // "mac", …); a different axis from the structural tree (§7.3/§8.6).
            const char *task_category;
        } method;
        struct {
            const struct class_desc *cls;
            bool indexed;
            // A non-owning reference edge (proposal §6.2/§6.3): the child is a
            // cross-reference the parent points at but does not own. It does
            // not cascade-delete and renders as a clickable link, not an
            // expandable child. Reference children are always callback-backed
            // (never in the attached/owning list) so cascade never frees them.
            bool reference;
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

// === Root object =============================================================
//
// The object tree has one root per process, named "emu". Unqualified
// paths resolve against it (so `cpu.pc` means `emu.cpu.pc`). The root
// is created on first access and persists for the process lifetime;
// machine setup attaches subsystem objects to it, and machine teardown
// detaches them. The root itself is never destroyed.

struct object *object_root(void);

// Free the root and any attached children. Used by tests and at
// process exit. After this returns, object_root() will lazily create
// a fresh, empty root on the next call.
void object_root_reset(void);

// Swap the root's class descriptor. Used by root_install to
// register the top-level root methods (proposal §5.10) while keeping
// the substrate's lazy-creation contract for object_root() — the
// initial namespace-only class lives in object.c so M1/M2 callers
// don't depend on root install order. After this call the
// resolver finds members declared on `cls` directly on the root, in
// addition to any runtime-attached children.
//
// Pass NULL to revert to the namespace-only default (used by
// object_root_reset paths in tests).
void object_root_set_class(const class_desc_t *cls);

// === Object lifetime =========================================================

// Construct an opaque object instance. instance_data is the back-pointer
// to module-private state (cpu_t*, scsi_t*, ...) that getters/setters
// cast through cls->instance_data. name is borrowed and must outlive the
// object. Returns NULL on allocation failure.
struct object *object_new(const class_desc_t *cls, void *instance_data, const char *name);

// Detach if attached, then free. Runs the per-object destructor (if set,
// see object_set_destructor) first so a module can release the C struct
// behind instance_data. Attached children are NOT recursively freed here;
// use object_delete_tree for cascade teardown.
void object_delete(struct object *o);

// Cascade-delete: free `o` and its entire owned subtree in post-order
// (deepest children first, then `o`). "Owned" means the attached-child
// (object_attach) edges, which form the spanning tree and the canonical
// path (proposal §6.1). Reference edges (member_t.child.reference, always
// callback-backed and never attached) are not followed. Indexed-collection
// item objects produced by member get/next callbacks are likewise not
// attached, so they are not freed here — their owning module frees them in
// its own destructor. Each object's destructor (object_set_destructor)
// runs before its wrapper memory is freed.
void object_delete_tree(struct object *o);

// Per-object destructor, run by object_delete / object_delete_tree just
// before the wrapper memory is freed (after invalidators fire and the meta
// node is released). The substrate always frees the object wrapper itself;
// the destructor only frees the module state behind instance_data (the
// cpu_t/scsi_t/image_t the wrapper points at) and any unattached
// collection-item wrappers the module owns. Default NULL (no-op), so
// existing callers that free their own structs are unaffected.
typedef void (*object_dtor_fn)(struct object *o);
void object_set_destructor(struct object *o, object_dtor_fn dtor);

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

// === Display label & ordering (proposal §7.1 / §7.4) ========================
//
// An object's `name` is its stable path segment (`machine`); its `label`
// is the human-facing display string ("Macintosh IIcx"). Hardware nodes
// are attached at runtime, so their label is set per-object here rather
// than on a static member. object_label falls back to the name when no
// label was set. The label string is borrowed and must outlive the object.
void object_set_label(struct object *o, const char *label);
const char *object_label(struct object *o); // label if set, else name

// Ordering weight for a deterministic SYSTEM tree. Lower sorts earlier;
// ties break on attach order. Default 0. Honoured by
// object_each_attached_ordered (and thus meta.children).
void object_set_order(struct object *o, int order);
int object_order(struct object *o);

// Visibility category for an attached child object (proposal §7.2). Per-
// member visibility lives in member_t.flags (M_CAT_*); attached hardware
// nodes carry it per-object here instead, since they are not declared
// members. Pass one of M_CAT_BASIC / M_CAT_ADVANCED / M_CAT_INTERNAL.
// Default M_CAT_BASIC (always shown).
void object_set_category(struct object *o, uint16_t category);
uint16_t object_category(struct object *o);

// Iterate this object's statically-attached children (named children
// added via object_attach). Calls fn for each. Indexed children declared
// via member_t.child.get/next are not visited here.
void object_each_attached(struct object *o, void (*fn)(struct object *parent, struct object *child, void *ud),
                          void *ud);

// Like object_each_attached, but visits children in ascending object_order
// (ties break on attach order), so the SYSTEM tab and meta.children render
// in a stable, meaningful sequence (proposal §7.4).
void object_each_attached_ordered(struct object *o, void (*fn)(struct object *parent, struct object *child, void *ud),
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

// === Named-argument binder ===================================================

// One named argument: a declared-slot name plus its value.
typedef struct {
    const char *name; // borrowed from the caller
    value_t value; // owned by the caller (binder aliases, never copies)
} named_arg_t;

// Maximum total bound arguments (mirrors the validator's scratch capacity).
#define OBJ_BIND_MAX_ARGS 16

// Bind a (positional list, named list) pair against a method's declared
// args[] table, producing the purely positional argv that node_call /
// node_validate_args consume. Rules (proposal-named-args-boot-config §3.1):
// positionals fill slots left to right; named args target declared fixed
// slots by name in any order; duplicates and unknown names are errors;
// OBJ_ARG_REST slots are positional-tail only. Unfilled interior slots are
// emitted as V_NONE holes, which the validator treats exactly like missing
// trailing arguments (default-fill, or "missing argument" error).
//
// out_argv must have capacity OBJ_BIND_MAX_ARGS. Bound values alias the
// caller's pos_argv/named values (holes own nothing), so the caller frees
// its originals as usual and must not free out_argv slots. Returns V_NONE
// on success, V_ERROR on a binding error.
value_t node_bind_args(node_t n, int pos_argc, const value_t *pos_argv, int named_n, const named_arg_t *named,
                       value_t *out_argv, int *out_argc);

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

// === Per-object invalidation hooks ==========================================
//
// Hot-path consumers that hold a pre-resolved node_t (proposal §9 — held
// breakpoint conditions, watch paths, …) need to be told when "their"
// node has gone away. The framework lets each object carry a small list
// of weak-reference callbacks; the entry's owner fires them on remove
// (via object_fire_invalidators) and listeners null their cached node.
//
// Invariants:
//  - register/unregister are O(N_listeners). N is small (≤ tens) in
//    practice, so a linear scan is fine.
//  - object_fire_invalidators runs every registered callback once, in
//    registration order, then clears the list. Idempotent on subsequent
//    calls (the list is empty after the first fire).
//  - object_delete fires invalidators automatically, then frees the
//    object — listeners may still be alive, but their cached node_t is
//    now invalid. The framework gives them the chance to react.
//
// The (cb, ud) pair identifies a listener for unregister; matching is
// done by exact pointer equality on both fields.

typedef void (*node_invalidate_fn)(void *ud);

void object_register_invalidator(struct object *o, node_invalidate_fn cb, void *ud);
void object_unregister_invalidator(struct object *o, node_invalidate_fn cb, void *ud);
void object_fire_invalidators(struct object *o);

// Verify class definition at registration time: every member name must
// be a valid identifier, must not collide with a reserved word, and
// must be unique within the class. Returns true on success; on failure
// writes a one-line message into err_buf (may be NULL).
bool object_validate_class(const class_desc_t *cls, char *err_buf, size_t err_size);

// === Meta-attribute slot ====================================================
//
// Each object carries an opaque pointer to its lazily-created `Meta`
// node (see meta.c). The slot accessors are the only path through
// which meta.c reads/writes the field without making struct object's
// layout public.
struct object *object_get_meta(struct object *o);
void object_set_meta(struct object *o, struct object *meta);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_OBJECT_H
