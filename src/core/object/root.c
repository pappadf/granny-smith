// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// root.c
// Defines the `emu` root class — the top-level introspection methods
// (objects / attributes / methods / help / time) plus a few thin
// wrappers (quit / echo) — and orchestrates the
// install/uninstall of the cfg-scoped stubs that hang off it: the shell
// namespace and its children here, and each subsystem's own nodes through
// the install hooks it registers (root_register_install).

#include "root.h"
#include "gs_out.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "alias.h"
#include "commands.h"
#include "object.h"
#include "shell.h"
#include "shell_funcs.h"
#include "system.h"
#include "usage.h"
#include "value.h"

// === Introspection root methods =============================================
// `objects`, `attributes`, `methods`, `help`, `time`. Each accepts an
// optional path string; empty / missing resolves to the root itself.

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
    struct object *c =
        n.member->child.collection ? object_entry_at(n.obj, n.member, n.index) : object_named_child(n.obj, n.member);
    return c ? c : n.obj;
}

// Growable V_STRING list used to accumulate object/attribute/method
// names for the introspection methods.
typedef struct {
    value_t *items;
    size_t len;
    size_t cap;
    bool oom; // set when a push failed; the list must not be returned short
} string_list_acc_t;

// Append `name` as a V_STRING through the shared accumulator.
//
// This was one of five near-identical {items, len, cap} doublers -- root.c,
// meta.c, alias.c, an inline one in object.c and another in debug.c -- beside
// val_list_push, which already existed and which object.c's copy already
// used.  Each copy had its own OOM behaviour, and four of the five DISCARDED
// the failure, so an allocation failure silently truncated the returned list
// instead of erroring: objects(), attributes(), methods(), meta.children and
// meta.indices would report a short list as if it were complete, which the
// inspector then renders as "these are all the members".  A truncated schema
// is worse than an error because the caller cannot tell.
static bool string_list_push(string_list_acc_t *acc, const char *name) {
    if (!name)
        return true;
    return val_list_push(&acc->items, &acc->len, &acc->cap, val_str(name));
}

static void each_attached_collect(struct object *parent, struct object *child, void *ud) {
    (void)parent;
    string_list_acc_t *acc = (string_list_acc_t *)ud;
    if (!string_list_push(acc, object_name(child)))
        acc->oom = true; // reported by the caller; see string_list_push
}

static DEF_METHOD(method_root_objects) {
    struct object *target = resolve_target(argc >= 1 ? &argv[0] : NULL);
    if (!target)
        return val_err("objects: path did not resolve");
    string_list_acc_t acc = {0};
    const class_desc_t *cls = object_class(target);
    if (cls) {
        for (size_t i = 0; i < cls->n_members; i++)
            if (cls->members[i].kind == M_CHILD)
                if (!string_list_push(&acc, cls->members[i].name))
                    acc.oom = true;
    }
    object_each_attached(target, each_attached_collect, &acc);
    if (acc.oom) {
        for (size_t i = 0; i < acc.len; i++)
            value_free(&acc.items[i]);
        free(acc.items);
        return val_err("out of memory");
    }
    return val_list(acc.items, acc.len);
}

static DEF_METHOD(method_root_attributes) {
    struct object *target = resolve_target(argc >= 1 ? &argv[0] : NULL);
    if (!target)
        return val_err("attributes: path did not resolve");
    string_list_acc_t acc = {0};
    const class_desc_t *cls = object_class(target);
    if (cls) {
        for (size_t i = 0; i < cls->n_members; i++)
            if (cls->members[i].kind == M_ATTR)
                if (!string_list_push(&acc, cls->members[i].name))
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

static DEF_METHOD(method_root_methods) {
    struct object *target = resolve_target(argc >= 1 ? &argv[0] : NULL);
    if (!target)
        return val_err("methods: path did not resolve");
    string_list_acc_t acc = {0};
    const class_desc_t *cls = object_class(target);
    if (cls) {
        for (size_t i = 0; i < cls->n_members; i++)
            if (cls->members[i].kind == M_METHOD)
                if (!string_list_push(&acc, cls->members[i].name))
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

// `help(path?)` — the usage text of any path (usage.c): a method's
// signature, arguments and doc; an attribute's type, value and doc; a node's
// doc and member lists.  The same text shell.usage returns.
static DEF_METHOD(method_root_help) {
    const char *path = (argc >= 1 && argv[0].s) ? argv[0].s : "";
    value_t v = object_usage_text(path);
    if (v.kind == V_ERROR) {
        value_free(&v);
        return val_err("help: path did not resolve");
    }
    return v;
}

// `time()` — wall-clock seconds since the Unix epoch. Useful for
// timestamping log lines from scripts; deterministic test runs use
// `rtc.time =` instead.
static DEF_METHOD(method_root_time) {
    return val_uint(8, (uint64_t)time(NULL));
}

// === Top-level wrappers =====================================================
// quit / echo. Subsystem-specific verbs live with
// their owning class (cpu.*, memory.*, debug.*, files.*, …); only the
// process-wide ones stay here.

// `quit()` — request emulator shutdown. Headless sets the script
// quit flag and stops the scheduler; in the browser, which owns the page's
// lifecycle, it says so rather than doing nothing silently.
static DEF_METHOD(method_root_quit) {
    if (gs_quit() != 0)
        return val_err("quit: not supported on this platform");
    return val_none();
}

// `echo(...)` — print arguments separated by spaces. Mirrors the
// classic `echo` shell command so test scripts can write the result
// of a `$(...)` expression to stdout without going through any
// detour. Returns true on success.
static DEF_METHOD(method_root_echo) {
    for (int i = 0; i < argc; i++) {
        if (i > 0)
            gs_outc(' ');
        switch (argv[i].kind) {
        case V_STRING:
            gs_outs(argv[i].s ? argv[i].s : "");
            break;
        case V_BOOL:
            gs_outs(argv[i].b ? "true" : "false");
            break;
        case V_INT:
            gs_outf("%lld", (long long)argv[i].i);
            break;
        case V_UINT:
            gs_outf("%llu", (unsigned long long)argv[i].u);
            break;
        case V_FLOAT:
            gs_outf("%g", argv[i].f);
            break;
        default:
            // Fall back to a path-form-style label for the kinds we
            // don't usually echo (V_OBJECT, V_LIST). Keeps output
            // deterministic for diff-based regression tests.
            gs_outs("<?>");
            break;
        }
    }
    gs_outc('\n');
    return val_bool(true);
}

static const arg_decl_t root_path_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Object path; empty resolves to the root"},
};
static const arg_decl_t root_help_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
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
     .doc = "Usage text of a path: signature, arguments, type, value, doc",
     .method = {.args = root_help_args, .nargs = 1, .result = V_STRING, .fn = method_root_help}    },
    {.kind = M_METHOD,
     .name = "time",
     .doc = "Wall-clock seconds since the Unix epoch",
     .method = {.args = NULL, .nargs = 0, .result = V_UINT, .fn = method_root_time}                },
    {.kind = M_METHOD,
     .name = "quit",
     .doc = "Exit the emulator (asks the legacy quit command to end the run)",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = method_root_quit}                },
    // `assert` is a statement keyword in shell v2 (script.c); the former
    // root method is gone — its name is now a reserved word.
    {.kind = M_METHOD,
     .name = "echo",
     .doc = "Print arguments separated by spaces (final newline appended)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_echo}                },
};

static const class_desc_t emu_root_class_real = {
    .name = "emu",
    .members = emu_root_members,
    .n_members = sizeof(emu_root_members) / sizeof(emu_root_members[0]),
};

// === Install / uninstall ====================================================
//
// Stubs are tied to a specific `cfg` pointer. Two lifecycle patterns
// have to work:
//
//   1. Cold boot:        system_create(new) → install(new); later
//                        system_destroy(new) → uninstall_if(new).
//   2. checkpoint --load: system_create(new) → install(new) runs
//                        BEFORE system_destroy(old) → uninstall_if(old).
//
// Pattern 2 needs the install on the new cfg to tear down the old
// stubs first (otherwise destroy(old) would wipe the freshly attached
// new stubs); and the destroy on the old cfg must be a no-op when
// install has already swapped to the new cfg. The g_installed_cfg
// pointer guards both directions.

#define MAX_STUBS 40
static struct object *g_stubs[MAX_STUBS];
static int g_stub_count = 0;
static struct config *g_installed_cfg = NULL;

// The registered subsystem install hooks, run in registration order.
#define MAX_INSTALL_HOOKS 8
static struct {
    root_install_fn install;
    root_uninstall_fn uninstall;
} g_hooks[MAX_INSTALL_HOOKS];
static int g_hook_count = 0;

void root_register_install(root_install_fn install, root_uninstall_fn uninstall) {
    if (!install)
        return;
    for (int i = 0; i < g_hook_count; i++)
        if (g_hooks[i].install == install)
            return; // already registered
    if (g_hook_count >= MAX_INSTALL_HOOKS) {
        fprintf(stderr, "root: install-hook table full (%d)\n", MAX_INSTALL_HOOKS);
        return;
    }
    g_hooks[g_hook_count].install = install;
    g_hooks[g_hook_count].uninstall = uninstall;
    g_hook_count++;
}

struct object *root_attach_stub(struct object *parent, struct object *o) {
    if (!o)
        return NULL;
    const class_desc_t *cls = object_class(o);
    if (g_stub_count >= MAX_STUBS) {
        // A discarded result makes a whole subtree quietly absent -- which
        // reads as a missing feature, not a resource limit -- so say so.
        fprintf(stderr, "root: stub table full (%d); '%s' not attached\n", MAX_STUBS,
                object_name(o) ? object_name(o) : "(unnamed)");
        object_delete(o);
        return NULL;
    }
    char err[200];
    if (!object_validate_class(cls, err, sizeof(err))) {
        fprintf(stderr, "root: class '%s' invalid: %s\n", cls && cls->name ? cls->name : "?", err);
        object_delete(o);
        return NULL;
    }
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

    // Top-level methods. Already installed by core_init via
    // root_install_class; the call is repeated here so paths that skip
    // core_init still get the methods.
    root_install_class();

    // Subsystem-scoped objects are registered by their owners (cpu_init,
    // memory_map_init, scc_init, rtc_init, via_init, scsi_init,
    // floppy_init, sound_init, debug_init). The platform-level facades
    // (mouse, screen, files, log, catalog) are process-singletons attached
    // from core_init, and the AppleTalk network's `appletalk` tree is
    // attached once by appletalk_network_init.
    //
    // What remains here is the Shell class instance with its children, and
    // then each registered subsystem hook (files.images, machine.nubus,
    // machine.pci).
    shell_class_register(cfg);
    for (int i = 0; i < g_hook_count; i++)
        g_hooks[i].install(cfg);
}

void root_uninstall(void) {
    // Detach function entry objects before their parent stub goes away.
    shell_funcs_uninstall();
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
    // teardown; a hook's own uninstall drops what it kept about its stubs.
    for (int i = 0; i < g_hook_count; i++)
        if (g_hooks[i].uninstall)
            g_hooks[i].uninstall();
    // The root method table is NOT reverted here, deliberately.
    //
    // It used to be, and that was a process-scoped global being undone by a
    // cfg-scoped teardown: after system_destroy -- a headless quit,
    // machine.boot's teardown, or a failed restore -- `echo`, `objects`,
    // `attributes`, `methods`, `help`, `time` and `quit` all stopped
    // resolving until a new machine existed.  The comment that stood here
    // feared "stale members", but the stale things are the STUBS, and the loop
    // above already detached them.  emu_root_class_real is a static descriptor
    // whose members take a path and walk the tree; not one of them holds or
    // dereferences a cfg, so there is nothing about it to go stale.
    // Aliases (built-in and user) survive machine teardown: they store
    // path text and re-resolve per access, so a
    // reference like `alias d = machine.floppy.drive[0]` tracks the
    // *new* drive object after a reboot instead of being wiped.
    g_installed_cfg = NULL;
}

void root_uninstall_if(struct config *cfg) {
    if (g_installed_cfg == cfg)
        root_uninstall();
}
