// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// object.h
// Object-model substrate: classes, members, objects, nodes, path resolution.
//
// Tests construct toy classes directly to exercise the resolver.

#ifndef GS_OBJECT_OBJECT_H
#define GS_OBJECT_OBJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
// OBJ_ARG_TEMPLATE — deferred-eval string slot: the
// command parser stores the raw, uninterpolated string body; the owning
// subsystem evaluates it later (possibly repeatedly) with extra
// bindings, e.g. logpoint messages with `$value`/`$addr`/`$size`.
#define OBJ_ARG_TEMPLATE 0x0020u
// OBJ_ARG_GROUPED — this optional argument is part of an all-or-nothing group
// that the method body checks by argument count (screen.match's exclude
// rectangles: one reference, or a reference plus all four edges, or plus all
// eight).  An optional argument with no default that a caller skips reaches
// the body as V_NONE; a grouped one may not be skipped alone -- naming a later
// argument past it is an error -- so the grouping lives in the declaration and
// not only in an `argc != 1 && argc != 5 && argc != 9` buried in the body.
#define OBJ_ARG_GROUPED 0x0040u
// OBJ_ARG_NONE_OK — the slot also takes `none`, meaning "unset": an
// attribute such as machine.scc.a.output is a path while it is set and
// `none` while it is not, so its setter accepts `none` to clear it and its
// getter may answer `none`.  Any other kind still has to match the slot.
#define OBJ_ARG_NONE_OK 0x0080u
// OBJ_ARG_POLY — a V_ANY / V_NONE argument that is intentionally
// polymorphic (a size given as a string or a count, say).  The doc lint
// (shell.lint_members) flags an untyped argument without it.
#define OBJ_ARG_POLY 0x0100u

// === Member visibility category ==============================================
//
// A three-tier visibility classification, the only content of member_t.flags
// (an attribute is read-only when it has no setter; display flags live in
// the slot's presentation_flags). Every consumer (SYSTEM tab, command
// browser) reads the category off the model so visibility cannot drift the
// way a hand-maintained allowlist did:
//   basic    — always shown in the tree; the default (0).
//   advanced — shown only with the "Advanced" toggle on.
//   internal — never shown in the tree, but fully scriptable.
#define M_CAT_BASIC    0x0000u // default — always shown
#define M_CAT_ADVANCED 0x0100u // shown only under the Advanced toggle
#define M_CAT_INTERNAL 0x0200u // never shown in UI; still scriptable
#define M_CAT_MASK     0x0300u // mask to extract the category bits

// === Method UI metadata ======================================================
//
// Flags on a method member that steer context menus and the command
// browser. They hang off the existing method descriptor; no new subsystem.
#define MM_DESTRUCTIVE 0x0001u // confirm before invoking (eject, rm, …)
#define MM_MUTATE      0x0002u // changes state (vs a pure query)
#define MM_HIDDEN      0x0004u // not surfaced in UI menus / browser
#define MM_IO          0x0008u // an I/O job: cost proportional to a file; answers later, reports progress, cancellable

// One declared parameter on a method.
//
// Two flag fields:
//   `validation_flags` change what the validator does (OBJ_ARG_*).
//   `presentation_flags` steer formatters and inspectors (VAL_HEX,
//      VAL_DEC, VAL_BIN, VAL_VOLATILE, VAL_SENSITIVE) — no effect at
//      call time on a method-arg slot, but propagated to help text.
typedef struct arg_decl {
    const char *name;
    // Declared kind. V_ANY (or, historically, V_NONE — the two are
    // equivalent here) accepts a value of any kind without coercion.
    value_kind_t kind;
    uint8_t width; // 1/2/4/8 for V_INT/V_UINT range check; 0 = unconstrained
    uint16_t validation_flags; // OBJ_ARG_OPTIONAL | OBJ_ARG_REST | OBJ_ARG_NONEMPTY | OBJ_ARG_STRICT_KIND
    uint16_t presentation_flags; // VAL_HEX | VAL_DEC | VAL_BIN | ...
    const char *const *enum_values; // NULL-terminated table for V_ENUM
    const value_t *default_value; // optional default for OBJ_ARG_OPTIONAL slots
    const char *doc;
    // What an omitted optional argument means when that is computed rather
    // than a value (`the current PC`, `the model's`).  Shown after the doc as
    // `; omitted: <default_doc>`; an argument has this or default_value.
    const char *default_doc;
} arg_decl_t;

// True if the argument declares a default worth showing: a value other than
// none or the empty string.
bool arg_has_default(const arg_decl_t *a);

// An argument's doc as a reader sees it: its doc, then `; omitted: …` for a
// computed default.  Writes into buf (NUL-terminated, truncated).
void arg_doc_text(const arg_decl_t *a, char *buf, size_t size);

// A NULL-terminated example list for member_t.examples.
#define EXAMPLES(...) ((const char *const[]){__VA_ARGS__, NULL})

// A required string argument naming a VFS path.
#define ARG_PATH(arg_name, arg_doc)                                                                                    \
    {.name = (arg_name), .kind = V_STRING, .presentation_flags = VAL_PATH, .doc = (arg_doc)}

// === Function pointer types ==================================================
//
// Attribute getters/setters and method callbacks all receive their own
// member descriptor so dispatchers shared across many members (e.g. the
// 471-entry `mac` class with one getter for every global) can recover
// per-member context from `m->user_data`.

struct member;
typedef value_t (*attr_get_fn)(struct object *self, const struct member *m);
// A setter OWNS `in`: it must value_free() it on every path, including its
// error returns (node_set frees `in` itself only when it rejects the write
// before reaching the setter).  Copy anything you need to keep — the value's
// string/bytes storage is heap memory that nothing else will release.  Scalar
// kinds carry no allocation, so a setter for those may skip the free.
typedef value_t (*attr_set_fn)(struct object *self, const struct member *m, value_t in);
typedef value_t (*method_fn)(struct object *self, const struct member *m, int argc, const value_t *argv);
typedef struct object *(*child_get_fn)(struct object *self, int index);
typedef int (*child_next_fn)(struct object *self, int prev_index);
typedef struct object *(*child_lookup_fn)(struct object *self, const char *name);
// Keyed-collection enumeration: the live key after `prev` (NULL to start), or
// NULL at the end.  The key stays valid while its entry exists.
typedef const char *(*child_next_key_fn)(struct object *self, const char *prev);

// === Collection descriptor ===================================================
//
// How a collection hands out its entries.  By index, the ids are sparse and
// stable (new entries take max_index_ever + 1; removed ids are never reused):
// get(i) answers each index in [0, slots), NULL for a hole, or -- for ids with
// no fixed bound -- next(prev) walks the live ones (-1 to start, -1 at the
// end) and get(i) answers each.  By key, lookup(key) answers an entry and
// next_key(prev) walks the live keys; keys are short identifiers
// (object_valid_key).  A collection may be both (`appletalk.afp.volumes`,
// whose lookup maps a volume name onto its slot).
//
// A collection is the one `entries` member of its container node's class
// (OBJ_ENTRIES); the container carries the collection verbs (add, clear,
// find).  A container with nothing but its entries is made from the
// descriptor alone by object_collection_new, which reads the container
// fields below.
typedef struct collection_desc {
    const struct class_desc *entry; // class of every entry
    struct {
        child_get_fn get;
        child_next_fn next;
        int slots;
    } by_index;
    struct {
        child_lookup_fn lookup;
        child_next_key_fn next_key;
    } by_key;
    // For object_collection_new: the container class's name and doc, the
    // entries member's doc, and further container members (collection
    // verbs).
    const char *name;
    const char *doc;
    const char *entries_doc;
    const struct member *verbs;
    size_t n_verbs;
} collection_desc_t;

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
    uint16_t flags; // M_CAT_* visibility
    // Optional display label. The path segment stays
    // `name`; the tree shows `label` when present, else `name`. NULL = use
    // the name.
    const char *label;
    // Ordering weight for a faithful, deterministic tree.
    // Lower sorts earlier; ties break on declaration order. Default 0.
    int16_t order;
    // Optional example statements (NULL-terminated), shown by help /
    // shell.usage as `e.g.  …`.
    const char *const *examples;
    union {
        struct {
            value_kind_t type;
            uint8_t width; // 1/2/4/8 for V_INT/V_UINT range check; 0 = unconstrained
            uint16_t validation_flags; // OBJ_ARG_NONEMPTY | OBJ_ARG_STRICT_KIND | OBJ_ARG_NONE_OK
            uint16_t presentation_flags; // VAL_HEX | VAL_DEC | VAL_BIN | VAL_VOLATILE | VAL_SENSITIVE
            const char *const *enum_values; // NULL-terminated table for V_ENUM
            attr_get_fn get;
            attr_set_fn set; // NULL → read-only
            const void *user_data; // borrowed; passed back via the `m` arg
        } attr;
        struct {
            const arg_decl_t *args;
            int nargs;
            // Declared result kind, checked by node_call in debug builds:
            //   V_NONE — the method returns nothing (only V_NONE/V_ERROR).
            //   V_ANY  — polymorphic: the kind depends on the arguments or
            //            on machine state (debug.mac.globals.read returns a
            //            uint for a 1/2/4-byte global, bytes for a wider
            //            one). Nothing is asserted about the kind.
            //   others — the result must carry exactly that kind (V_ERROR
            //            is always allowed, as the in-band error path).
            value_kind_t result;
            method_fn fn;
            // UI metadata: MM_DESTRUCTIVE | MM_MUTATE | MM_HIDDEN | MM_IO.
            uint16_t ui_flags;
            // Short verb shown in menus ("Save image…") when distinct from
            // the method name ("export"). NULL = use the method name.
            const char *verb_label;
            // Optional one-line description of the result, for a method
            // whose result kind alone says little (V_ANY, V_MAP, V_LIST).
            const char *result_doc;
        } method;
        struct {
            // A named child: its class, and lookup(self, name)→object|NULL
            // when the child is not statically attached.
            const struct class_desc *cls;
            child_lookup_fn lookup;
            // A collection's entries (the container's `entries` member):
            // the descriptor that hands them out.  A class has at most one.
            const struct collection_desc *collection;
            // A non-owning reference edge: the child is a
            // cross-reference the parent points at but does not own. It does
            // not cascade-delete and renders as a clickable link, not an
            // expandable child. Reference children are always callback-backed
            // (never in the attached/owning list) so cascade never frees them.
            bool reference;
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
    const char *doc; // one sentence describing a node of this class; NULL = none
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
// register the top-level root methods while keeping
// the substrate's lazy-creation contract for object_root() — the
// initial namespace-only class lives in object.c so early callers
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
// path. Reference edges (member_t.child.reference, always
// callback-backed and never attached) are not followed. Collection entries
// are likewise not attached, so they are not freed here — their owning
// module (or its object_cache) frees them. Each object's destructor (object_set_destructor)
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
// attached list; collection entries are handed out by the collection
// descriptor instead.
void object_attach(struct object *parent, struct object *child);
void object_detach(struct object *child);

// Borrowed accessors.
const class_desc_t *object_class(const struct object *o);
const char *object_name(const struct object *o);
void *object_data(struct object *o);
struct object *object_parent(struct object *o);

// === Logical parents =========================================================
//
// Objects handed out by a child callback -- collection entries (get/lookup)
// and lookup-backed named children such as drive[n].disk -- are never
// attached, so they have no parent and, without this, no path.  Their
// creator registers the node whose member produces them: a non-owning
// back-link (not added to the parent's child list, not cascade-deleted),
// cleared automatically if the parent is freed first.  Exactly one of
// `name` (a named lookup child), `index >= 0` (an indexed entry) or `key` (a
// keyed entry) is given; `name` and `key` are copied.  The path is then
// `<parent>.<name>`, `<parent>[<index>]` or `<parent>["<key>"]`.  Passing a
// NULL parent clears the link.
void object_set_logical_parent(struct object *obj, struct object *parent, const char *name, int index, const char *key);
struct object *object_logical_parent(struct object *o); // NULL when none
const char *object_logical_name(struct object *o); // named-child segment, or NULL
int object_logical_index(struct object *o); // entry index, or -1
const char *object_logical_key(struct object *o); // entry key, or NULL

// Room for any canonical path (object_compute_path truncates past it).
#define OBJ_PATH_MAX 512

// Keys of keyed collections are short identifiers: [A-Za-z0-9_.-]{1,63}.
#define OBJ_KEY_MAX 63
bool object_valid_key(const char *key);

// === Collections =============================================================

// A container's `entries` member over collection descriptor `coll`.
#define OBJ_ENTRIES(coll, doc_text)                                                                                    \
    {                                                                                                                  \
        .kind = M_CHILD, .name = "entries", .doc = (doc_text), .child = {.collection = (coll) }                        \
    }

// True if `m` is a collection's entries member.
static inline bool member_is_collection(const member_t *m) {
    return m && m->kind == M_CHILD && m->child.collection;
}

// The collection member of a class (its one `entries` member), or NULL: what
// makes a node a collection container.
const member_t *class_collection(const class_desc_t *cls);

// A container node over `coll` with no class of its own: object_new of a
// class built once per descriptor from its container fields (name, doc,
// entries_doc, verbs).
struct object *object_collection_new(const collection_desc_t *coll, void *data, const char *name);

// Entry `index` / `key` of collection member `m` on `self`, or NULL.  An entry
// handed out without a logical parent is given `self` as one.
struct object *object_entry_at(struct object *self, const member_t *m, int index);
struct object *object_entry_by_key(struct object *self, const member_t *m, const char *key);

// The object named child member `m` stands for right now: its lookup (given a
// logical parent as above), else the attached child of that name; or NULL.
struct object *object_named_child(struct object *self, const member_t *m);

// The next live index after `prev` (-1 to start), or -1: the collection's
// next(), or a walk of get() over its slots.
int object_child_next(struct object *self, const member_t *m, int prev);

// The next live key after `prev` (NULL to start), or NULL.
const char *object_child_next_key(struct object *self, const member_t *m, const char *prev);

// Live entries of collection member `m`: its indices, or for a collection
// that is keyed only, its keys.
uint32_t object_collection_count(struct object *self, const member_t *m);

// === Entry caches ============================================================
//
// The entry objects of one collection, made on first use and found again by
// index or key.  Each is an object_new of the cache's class and name over the
// `data` given when it is made, with the container (object_cache_set_parent,
// before or after) as its logical parent; the cache owns them.  A collection
// whose records come and go frees the entries of dead ones with
// object_cache_sweep; object_cache_clear frees them all, with any subtree
// attached to them.
typedef struct object_cache {
    const class_desc_t *cls; // entry class
    const char *name; // entry object name (borrowed), may be NULL
    struct object *parent; // the container; cleared if it is freed first
    struct object_cache_slot *slots; // heap: one per entry made
    int n, cap;
} object_cache_t;

#define OBJECT_CACHE(entry_cls, entry_name) {.cls = (entry_cls), .name = (entry_name)}

struct object *object_cache_at(object_cache_t *c, int index, void *data); // find or make
struct object *object_cache_key(object_cache_t *c, const char *key, void *data); // find or make
struct object *object_cache_find(const object_cache_t *c, int index); // NULL when not made
void object_cache_set_parent(object_cache_t *c, struct object *parent);
// Free every entry for which live(entry, ud) is false.
void object_cache_sweep(object_cache_t *c, bool (*live)(struct object *entry, void *ud), void *ud);
void object_cache_clear(object_cache_t *c);

// The index an entry was made at by its cache, or -1.
int object_entry_index(struct object *entry);

// === Counter blocks ==========================================================
//
// A stats object publishes a struct of uint64_t counters, one read-only
// attribute per field, each member naming its field by offset.  One getter
// serves every such block: OBJ_U64_FIELD reads the block the object's data
// points to (object_new(cls, block, name)); OBJ_U64_FIELD_WITH takes a getter
// of its own, for a block that moves -- which returns obj_u64_at(block, m).
value_t obj_u64_at(const void *block, const member_t *m);
value_t obj_u64_field_get(struct object *self, const member_t *m);

#define OBJ_U64_FIELD_WITH(block_type, field, doc_text, getter)                                                        \
    {                                                                                                                  \
        .kind = M_ATTR, .name = #field, .doc = doc_text, .attr = {                                                     \
            .type = V_UINT,                                                                                            \
            .width = 8,                                                                                                \
            .get = getter,                                                                                             \
            .user_data = (const void *)(uintptr_t)offsetof(block_type, field)                                          \
        }                                                                                                              \
    }
#define OBJ_U64_FIELD(block_type, field, doc_text) OBJ_U64_FIELD_WITH(block_type, field, doc_text, obj_u64_field_get)

// === Display label & ordering ================================================
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

// Visibility category for an attached child object. Per-
// member visibility lives in member_t.flags (M_CAT_*); attached hardware
// nodes carry it per-object here instead, since they are not declared
// members. Pass one of M_CAT_BASIC / M_CAT_ADVANCED / M_CAT_INTERNAL.
// Default M_CAT_BASIC (always shown).
void object_set_category(struct object *o, uint16_t category);
uint16_t object_category(struct object *o);

// One-sentence description of this node.  object_doc answers the object's
// own doc, else its class's, else "" -- never NULL.  The string is borrowed
// and must outlive the object.
void object_set_doc(struct object *o, const char *doc);
const char *object_doc(struct object *o);

// Domain of a root child: what the top-level node *is*.  Drives the dividers
// in the SYSTEM tab and at the command browser's root.
#define OBJ_DOMAIN_EMULATOR 0 // default
#define OBJ_DOMAIN_MACHINE  1
#define OBJ_DOMAIN_NETWORK  2
void object_set_domain(struct object *o, uint8_t domain);
uint8_t object_domain(struct object *o);
const char *object_domain_name(uint8_t domain); // "emulator" | "machine" | "network"

// Iterate this object's statically-attached children (named children
// added via object_attach). Calls fn for each. Indexed children declared
// by a collection descriptor are not visited here.
void object_each_attached(struct object *o, void (*fn)(struct object *parent, struct object *child, void *ud),
                          void *ud);

// Like object_each_attached, but visits children in ascending object_order
// (ties break on attach order), so the SYSTEM tab and meta.children render
// in a stable, meaningful sequence.
void object_each_attached_ordered(struct object *o, void (*fn)(struct object *parent, struct object *child, void *ud),
                                  void *ud);

// Linear lookup of a member by name. Returns NULL if not found.
const member_t *class_find_member(const class_desc_t *cls, const char *name);

// === Member predicates =======================================================
//
// The one reading of each question every surface asks of a member (meta,
// usage, lint, completion).

// Read-only: an attribute with no setter.
static inline bool member_is_readonly(const member_t *m) {
    return m->kind == M_ATTR && !m->attr.set;
}

// Shown in the basic tier: category basic, and not a hidden method.
static inline bool member_is_basic(const member_t *m) {
    return (m->flags & M_CAT_MASK) == M_CAT_BASIC && !(m->kind == M_METHOD && (m->method.ui_flags & MM_HIDDEN));
}

// Listed on its node (help's member lists): not internal, and not a hidden
// method.
static inline bool member_is_listed(const member_t *m) {
    return (m->flags & M_CAT_MASK) != M_CAT_INTERNAL && !(m->kind == M_METHOD && (m->method.ui_flags & MM_HIDDEN));
}

// === Tree walk ===============================================================
//
// One walk of the live tree for every tool that visits all of it (the doc
// lint): from `start`, each object once -- then its declared members, its
// named children, its collection entries and its attached children, depth
// first -- with its canonical path (as meta.path gives it; "" for the root).
typedef struct object_visitor {
    // An object reached.  `via` is the child member it was reached through
    // (NULL for the start and attached children); `basic` whether it shows
    // in the basic tier (an entry or a named child's subtree does not);
    // `first` whether it is the first object of its class the walk met.
    // Return false to skip its members and subtree.
    bool (*object)(struct object *o, const member_t *via, const char *path, bool basic, bool first, void *ud);
    // A declared member of an object reached; `path` is the member's path.
    void (*member)(struct object *o, const member_t *m, const char *path, bool first, void *ud);
} object_visitor_t;

void object_walk(struct object *start, const object_visitor_t *v, void *ud);

// === Node and resolution =====================================================

// Resolution returns a node = (object, member, index). The node is the
// single entity the shell, JSON bridge, scripts, and held breakpoint
// references all carry.
typedef struct node {
    struct object *obj;
    const member_t *member; // NULL if the path resolves to an object itself
    int index; // entry index on a collection member; -1 otherwise
} node_t;

// True if the node is bound to something resolvable.
static inline bool node_valid(node_t n) {
    return n.obj != NULL;
}

// Resolve a dotted path string against `root`. The path may include
// segments of the form `name`, `[index]`, or `.index`. Returns a node
// with obj=NULL on failure. Paths only — no method calls
// (those are the shell's job to assemble); the resolver returns the
// method's member in `member` so the caller can node_call it.
node_t object_resolve(struct object *root, const char *path);

// Read / write / call by node. node_get on an M_CHILD member returns
// V_OBJECT pointing at the child; on M_METHOD returns V_ERROR (use
// node_call). node_set on a read-only attribute returns V_ERROR.
// node_set consumes `v`: it frees the value when it rejects the write, and
// otherwise hands ownership to the attribute's setter (see attr_set_fn).
// Callers must not free `v` afterwards.
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
// node_validate_args consume. Rules:
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

// Key descent into a collection: `volumes["Shared"]`. Routed to the
// collection's by_key lookup, which maps a stable name onto whatever entry
// currently holds it. Returns an invalid node when the collection has no
// lookup or the key is unknown.
node_t node_child_key(node_t n, const char *key);

// === Reserved-word check =====================================================

// Reserved words may not be used as member names, alias names, or any
// future user-bindable identifier. Members: boolean-literal spellings +
// script-grammar keywords.
//
// Returns true if `name` collides with a reserved word.
bool object_is_reserved_word(const char *name);

// The shell's keywords, in table order, each with a one-line syntax: the
// reserved words plus contextual ones (`command`, a keyword only in its
// statement shape).  `is_statement`: it heads a statement.
size_t object_keyword_count(void);
const char *object_keyword(size_t i);
const char *object_keyword_syntax(size_t i);
bool object_keyword_is_statement(size_t i);

// Validate a candidate member/alias name. Returns true if acceptable.
// Diagnostic messages are written to err_buf (may be NULL).
bool object_validate_name(const char *name, char *err_buf, size_t err_size);

// === Per-object invalidation hooks ==========================================
//
// Hot-path consumers that hold a pre-resolved node_t (held
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

// Documentation gaps a member declares, beside the hard errors above: a
// basic-tier method argument with no doc, an untyped (V_ANY / V_NONE)
// argument without OBJ_ARG_POLY, a V_ANY result without result_doc.  Calls
// report once per gap; `a` is the argument, or NULL for a member rule.
typedef void (*object_doc_gap_fn)(const member_t *m, const arg_decl_t *a, const char *rule, void *ud);
void object_member_doc_gaps(const member_t *m, object_doc_gap_fn report, void *ud);

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
