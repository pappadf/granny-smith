// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// root.c
// Defines the `emu` root class — the top-level introspection methods
// (objects / attributes / methods / help / time) plus a few thin
// wrappers (quit / assert / echo / download) — and orchestrates the
// install/uninstall of the small set of cfg-scoped stubs that hang off
// it (the shell namespace and shell.alias child, the storage view of
// cfg->images).
//
// Subsystem-owned classes (cpu/fpu/memory/scc/rtc/via/scsi/floppy/sound/
// appletalk/debug/mouse/keyboard/screen/vfs/find/storage*) live in their
// owning modules and self-register via *_init / *_delete. Built-in
// `$reg` aliases for CPU/FPU registers register from cpu_init().

#include "root.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "alias.h"
#include "debug.h"
#include "object.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

// instance_data on these stubs is config_t*. The lifetime is bounded
// by root_install / root_uninstall — same scope as the owning emulator
// instance.
//
// Class descriptors are defined in their owning modules (cpu.c,
// memory.c, machine.c, scc.c, etc.) and referenced here via extern
// to keep this file as the install/uninstall orchestrator.

extern const class_desc_t storage_class_real; // src/core/storage/storage.c
extern const class_desc_t storage_images_collection_class; // src/core/storage/storage.c
extern const class_desc_t shell_alias_class; // src/core/object/alias.c

// === Shell stub ==============================================================
// Empty class so `shell` exists as a path component for shell.alias.* etc.
// without forcing a real subsystem hookup at install time.

static const class_desc_t shell_class_desc = {.name = "shell", .members = NULL, .n_members = 0};

// Introspection root methods: `objects`, `attributes`, `methods`,
// `help`, `time`. Each accepts an optional path string; empty / missing
// resolves to the root itself.

static struct object *resolve_target(const value_t *path_arg) {
    const char *path = (path_arg && path_arg->kind == V_STRING && path_arg->s) ? path_arg->s : "";
    node_t n = object_resolve(object_root(), path);
    if (!node_valid(n))
        return NULL;
    // For attribute / method nodes we report on the parent object's
    // class. For object-typed nodes (M_CHILD or named children) we
    // descend to the target object.
    if (!n.member)
        return n.obj;
    if (n.member->kind != M_CHILD)
        return n.obj;
    if (n.member->child.indexed) {
        if (n.index < 0 || !n.member->child.get)
            return n.obj;
        struct object *c = n.member->child.get(n.obj, n.index);
        return c ? c : n.obj;
    }
    if (n.member->child.lookup) {
        struct object *c = n.member->child.lookup(n.obj, n.member->name);
        if (c)
            return c;
    }
    return n.obj;
}

// Append `name` as a V_STRING into a growing items array. Returns
// false on allocation failure (caller falls through to V_LIST with
// what's been accumulated).
typedef struct {
    value_t *items;
    size_t len;
    size_t cap;
} string_list_acc_t;

static bool string_list_push(string_list_acc_t *acc, const char *name) {
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

// Walks the class member table, pushing names of members matching
// `kind`. Plus, for kind == M_CHILD, also enumerates runtime-attached
// named children (object_each_attached).
typedef struct {
    string_list_acc_t *acc;
    member_kind_t kind; // 0 (M_ATTR base) means "any"
} class_walker_t;

static void each_attached_collect(struct object *parent, struct object *child, void *ud) {
    (void)parent;
    string_list_acc_t *acc = (string_list_acc_t *)ud;
    string_list_push(acc, object_name(child));
}

static value_t method_root_objects(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    struct object *target = resolve_target(argc >= 1 ? &argv[0] : NULL);
    if (!target)
        return val_err("objects: path did not resolve");
    string_list_acc_t acc = {0};
    const class_desc_t *cls = object_class(target);
    if (cls) {
        for (size_t i = 0; i < cls->n_members; i++)
            if (cls->members[i].kind == M_CHILD)
                string_list_push(&acc, cls->members[i].name);
    }
    object_each_attached(target, each_attached_collect, &acc);
    return val_list(acc.items, acc.len);
}

static value_t method_root_attributes(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    struct object *target = resolve_target(argc >= 1 ? &argv[0] : NULL);
    if (!target)
        return val_err("attributes: path did not resolve");
    string_list_acc_t acc = {0};
    const class_desc_t *cls = object_class(target);
    if (cls) {
        for (size_t i = 0; i < cls->n_members; i++)
            if (cls->members[i].kind == M_ATTR)
                string_list_push(&acc, cls->members[i].name);
    }
    return val_list(acc.items, acc.len);
}

static value_t method_root_methods(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    struct object *target = resolve_target(argc >= 1 ? &argv[0] : NULL);
    if (!target)
        return val_err("methods: path did not resolve");
    string_list_acc_t acc = {0};
    const class_desc_t *cls = object_class(target);
    if (cls) {
        for (size_t i = 0; i < cls->n_members; i++)
            if (cls->members[i].kind == M_METHOD)
                string_list_push(&acc, cls->members[i].name);
    }
    return val_list(acc.items, acc.len);
}

// `help(path?)` — return the doc string of the resolved member. For
// object-typed nodes, returns the class name (no separate "class doc"
// field exists in the substrate yet).
static value_t method_root_help(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *path = (argc >= 1 && argv[0].kind == V_STRING && argv[0].s) ? argv[0].s : "";
    node_t n = object_resolve(object_root(), path);
    if (!node_valid(n))
        return val_err("help: path did not resolve");
    if (n.member && n.member->doc)
        return val_str(n.member->doc);
    if (n.member)
        return val_str(n.member->name ? n.member->name : "");
    const class_desc_t *cls = object_class(n.obj);
    return val_str(cls && cls->name ? cls->name : "");
}

// `time()` — wall-clock seconds since the Unix epoch. Useful for
// timestamping log lines from scripts; deterministic test runs use
// `rtc.time =` instead.
static value_t method_root_time(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    return val_uint(8, (uint64_t)time(NULL));
}

// --- Top-level methods that wrap shell-style verbs -----------------------
//
// These flatten existing `image foo` / `hd foo` / `rom foo` / `vrom foo`
// / `cp` / `quit` shell forms into one-call methods. Each wrapper calls
// the underlying C primitive directly (or `shell_<cmd>_argv` for the
// rich parsers).
//
// `let` and `source` are not wrapped here — they touch shell-state
// plumbing (variables, script context) that lives in the shell layer.
// `hd_download` similarly requires platform/network state outside the
// object tree.

// `quit()` — request emulator shutdown. Headless sets the script
// quit flag and stops the scheduler; WASM is a no-op (the browser
// owns the lifecycle).
static value_t method_root_quit(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    gs_quit();
    return val_none();
}

// `assert(predicate, [message])` — proposal §2.5 truthy check. Called
// in path-form as `assert <pred>` or `assert <pred> "<msg>"`. The
// caller has already $()-expanded the predicate to a string, so the
// method just runs predicate_is_truthy on it; on failure the script
// aborts via the val_err return path (path-form prints to stderr and
// returns -1, which the headless runner treats as fatal).
static value_t method_root_assert(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("assert: expected (predicate, [message])");
    const char *pred = (argv[0].kind == V_STRING && argv[0].s) ? argv[0].s : NULL;
    if (!pred && argv[0].kind == V_BOOL)
        pred = argv[0].b ? "true" : "false";
    if (!pred)
        pred = "";
    const char *msg = (argc >= 2 && argv[1].kind == V_STRING) ? argv[1].s : NULL;
    if (predicate_is_truthy(pred)) {
        printf("ASSERT OK: %s\n", pred);
        return val_bool(true);
    }
    if (msg && *msg)
        printf("ASSERT FAILED: %s\n", msg);
    else
        printf("ASSERT FAILED: %s\n", pred);
    return val_err("assert failed: %s", msg && *msg ? msg : pred);
}

// `echo(...)` — print arguments separated by spaces. Mirrors the
// classic `echo` shell command so test scripts can write the result
// of a `$(...)` expression to stdout without going through any
// detour. Returns true on success.
static value_t method_root_echo(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    for (int i = 0; i < argc; i++) {
        if (i > 0)
            putchar(' ');
        switch (argv[i].kind) {
        case V_STRING:
            fputs(argv[i].s ? argv[i].s : "", stdout);
            break;
        case V_BOOL:
            fputs(argv[i].b ? "true" : "false", stdout);
            break;
        case V_INT:
            printf("%lld", (long long)argv[i].i);
            break;
        case V_UINT:
            printf("%llu", (unsigned long long)argv[i].u);
            break;
        case V_FLOAT:
            printf("%g", argv[i].f);
            break;
        default:
            // Fall back to a path-form-style label for the kinds we
            // don't usually echo (V_OBJECT, V_LIST). Keeps output
            // deterministic for diff-based regression tests.
            fputs("<?>", stdout);
            break;
        }
    }
    putchar('\n');
    return val_bool(true);
}

// `download(path)` — trigger a browser file download. Routes to the
// platform-specific gs_download (WASM streams via Blob+anchor; headless
// prints a "not supported" stub).
static value_t method_root_download(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("download: expected (path)");
    return val_bool(gs_download(argv[0].s ? argv[0].s : "") == 0);
}

static const arg_decl_t root_path_arg[] = {
    {.name = "path", .kind = V_STRING, .doc = "File path"},
};
static const arg_decl_t root_path_args[] = {
    {.name = "path", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Object path; empty resolves to the root"},
};
static const arg_decl_t root_help_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .flags = OBJ_ARG_OPTIONAL,
     .doc = "Path to a member or object; empty resolves to the root"},
};
static const member_t emu_root_members[] = {
    {.kind = M_METHOD,
     .name = "objects",
     .doc = "List child object names at the given path (or root)",
     .method = {.args = root_path_args, .nargs = 1, .result = V_LIST, .fn = method_root_objects}   },
    {.kind = M_METHOD,
     .name = "attributes",
     .doc = "List attribute names of the resolved object's class",
     .method = {.args = root_path_args, .nargs = 1, .result = V_LIST, .fn = method_root_attributes}},
    {.kind = M_METHOD,
     .name = "methods",
     .doc = "List method names of the resolved object's class",
     .method = {.args = root_path_args, .nargs = 1, .result = V_LIST, .fn = method_root_methods}   },
    {.kind = M_METHOD,
     .name = "help",
     .doc = "Return the doc string of a resolved member (or class name)",
     .method = {.args = root_help_args, .nargs = 1, .result = V_STRING, .fn = method_root_help}    },
    {.kind = M_METHOD,
     .name = "time",
     .doc = "Wall-clock seconds since the Unix epoch",
     .method = {.args = NULL, .nargs = 0, .result = V_UINT, .fn = method_root_time}                },
    {.kind = M_METHOD,
     .name = "quit",
     .doc = "Exit the emulator (asks the legacy quit command to end the run)",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = method_root_quit}                },
    {.kind = M_METHOD,
     .name = "assert",
     .doc = "Assert that a predicate is truthy; abort the script otherwise",
     .method = {.args = NULL, .nargs = 2, .result = V_BOOL, .fn = method_root_assert}              },
    {.kind = M_METHOD,
     .name = "echo",
     .doc = "Print arguments separated by spaces (final newline appended)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_echo}                },
    {.kind = M_METHOD,
     .name = "download",
     .doc = "Trigger a browser file download (WASM-only)",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_download}   },
};

static const class_desc_t emu_root_class_real = {
    .name = "emu",
    .members = emu_root_members,
    .n_members = sizeof(emu_root_members) / sizeof(emu_root_members[0]),
};

// === Install / uninstall ====================================================
//
// Lifecycle invariant: stubs are tied to a specific `cfg` pointer. Two
// patterns both have to work:
//
//   1. Cold boot
//        system_create(new) -> root_install(new)
//        ... later ...
//        system_destroy(new) -> root_uninstall_if(new)
//
//   2. checkpoint --load (cmd_load_checkpoint)
//        new = system_restore(...);     // system_create(new) -> install(new)
//        global_emulator = new;
//        system_destroy(old)            // uninstall_if(old) -- no-op
//
// The danger is pattern 2 with a naive idempotent install: if install
// short-circuits when stubs already exist, the install for `new` is a
// no-op (old's stubs are still attached), and the subsequent
// destroy(old) wipes everything — leaving the object root empty and
// every gsEval('cpu.pc' / 'running' / …) returning
// `{"error":"path '...' did not resolve"}`.
//
// The fix: track which cfg the stubs were installed for. Install is
// idempotent only for the *same* cfg and otherwise uninstalls before
// reinstalling. `root_uninstall_if(cfg)` (called from system_destroy)
// only tears down when the stubs are still associated with `cfg` — so
// destroying the old config after a load is a no-op.

#define MAX_STUBS 40
static struct object *g_stubs[MAX_STUBS];
static int g_stub_count = 0;
static struct config *g_installed_cfg = NULL;

static struct object *attach_stub(struct object *parent, const class_desc_t *cls, void *data, const char *name) {
    if (g_stub_count >= MAX_STUBS)
        return NULL;
    char err[200];
    if (!object_validate_class(cls, err, sizeof(err))) {
        fprintf(stderr, "root: class '%s' invalid: %s\n", cls->name ? cls->name : "?", err);
        return NULL;
    }
    struct object *o = object_new(cls, data, name);
    if (!o)
        return NULL;
    object_attach(parent ? parent : object_root(), o);
    g_stubs[g_stub_count++] = o;
    return o;
}

void root_install_class(void) {
    // Registers the top-level method table on the object root. Safe to
    // call repeatedly — object_root_set_class is idempotent for the
    // same class pointer.
    object_root_set_class(&emu_root_class_real);
}

void root_install(struct config *cfg) {
    // Idempotent for the SAME cfg — second-call from a redundant init
    // path keeps the existing stubs.
    if (g_stub_count > 0 && g_installed_cfg == cfg)
        return;
    // Different cfg (typically: checkpoint --load just produced a new
    // config). Tear down the old stubs before attaching new ones, so
    // child objects don't dangle pointers into freed config state and
    // the eventual `system_destroy(old_cfg)` call below doesn't end up
    // wiping the freshly installed root.
    if (g_stub_count > 0)
        root_uninstall();
    g_installed_cfg = cfg;

    // Top-level methods. Already installed by shell_init via
    // root_install_class; the call is repeated here so paths that skip
    // shell_init still get the methods.
    root_install_class();

    // Subsystem-scoped objects are registered by their owners (cpu_init,
    // memory_map_init, scc_init, rtc_init, via_init, scsi_init,
    // floppy_init, sound_init, appletalk_init, debug_init). The
    // platform-level facades (mouse, keyboard, screen, vfs, find) are
    // process-singletons attached from shell_init via their owning
    // module's *_class_register hook.
    //
    // What remains here is the shell namespace stub, the storage view
    // of cfg->images, and the shell.alias child.
    struct object *shell_obj = attach_stub(NULL, &shell_class_desc, cfg, "shell");
    struct object *storage_obj = attach_stub(NULL, &storage_class_real, cfg, "storage");
    if (storage_obj) {
        attach_stub(storage_obj, &storage_images_collection_class, cfg, "images");
        storage_object_classes_init(cfg);
    }

    // shell.alias child object.
    if (shell_obj)
        attach_stub(shell_obj, &shell_alias_class, cfg, "alias");
}

void root_uninstall(void) {
    for (int i = g_stub_count - 1; i >= 0; i--) {
        struct object *o = g_stubs[i];
        if (o) {
            object_detach(o);
            object_delete(o);
        }
        g_stubs[i] = NULL;
    }
    g_stub_count = 0;
    // Subsystem-scoped entries (scsi/floppy/atalk-share/cpu/etc) are
    // torn down by their owning *_delete functions during machine
    // teardown. Only the cfg-scoped storage.images entry array is freed
    // here.
    storage_object_classes_teardown();
    // Restore the namespace-only root class so a fresh object_root()
    // call after uninstall doesn't surface stale members.
    object_root_set_class(NULL);
    alias_reset();
    g_installed_cfg = NULL;
}

// Conditional uninstall used by `system_destroy(cfg)`: only tears down
// when the stubs are still associated with `cfg`. After
// `checkpoint --load`, the order is `system_create(new)` → install(new)
// → `system_destroy(old)`; without this gate the destroy would wipe the
// freshly installed `new` root.
void root_uninstall_if(struct config *cfg) {
    if (g_installed_cfg == cfg)
        root_uninstall();
}
