// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gs_classes.c
// Object-model install/uninstall orchestrator + a few process-/cfg-
// scoped class definitions that have no per-subsystem owner:
//   - scheduler / shell / storage namespace stubs
//   - shell.alias methods
//   - root-level methods (cp / peeler / rom_load / hd_attach / …)
//   - top-level introspection (objects/attributes/methods/help/print/time)
//   - debug-thin wrapper methods used by the typed bridge
//   - built-in cpu / fpu register aliases (e.g. $pc, $d0, $fpcr)
//
// Subsystem-owned classes (cpu/fpu/memory/scc/rtc/via/scsi/floppy/
// sound/appletalk/debug/mouse/keyboard/screen/vfs/find/storage_image)
// live in their owning modules and self-register via *_init / *_delete.

#include "gs_classes.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "addr_format.h"
#include "alias.h"
#include "appletalk.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "debug.h"
#include "debug_mac.h"
#include "drive_catalog.h"
#include "floppy.h"
#include "fpu.h"
#include "image.h"
#include "image_apm.h"
#include "image_vfs.h"
#include "keyboard.h"
#include "machine.h"
#include "memory.h"
#include "object.h"
#include "peeler.h"
#include "peeler_shell.h"
#include "rom.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "shell.h"
#include "sound.h"
#include "system.h"
#include "system_config.h"
#include "value.h"
#include "vfs.h"
#include "via.h"

// instance_data on these stubs is config_t*. The lifetime is bounded
// by gs_classes_install / gs_classes_uninstall — same scope as the
// owning emulator instance.
//
// Class descriptors are defined in their owning modules (cpu.c,
// memory.c, machine.c, scc.c, etc.) and referenced here via extern
// to keep this file as the install/uninstall orchestrator.

extern const class_desc_t machine_class; // src/machines/machine.c
extern const class_desc_t mouse_class; // src/core/peripherals/mouse.c
extern const class_desc_t keyboard_class; // src/core/peripherals/adb.c
extern const class_desc_t vfs_class; // src/core/vfs/vfs.c
extern const class_desc_t find_class; // src/core/debug/cmd_find.c
extern const class_desc_t screen_class; // src/core/debug/debug.c
extern const class_desc_t storage_class_real; // src/core/storage/storage.c
extern const class_desc_t storage_image_class; // src/core/storage/storage.c
extern const class_desc_t storage_images_collection_class; // src/core/storage/storage.c
extern const class_desc_t shell_alias_class; // src/core/object/alias.c

// === Scheduler / Shell / Storage stubs ======================================

static const class_desc_t scheduler_class_desc = {.name = "scheduler", .members = NULL, .n_members = 0};
static const class_desc_t shell_class_desc = {.name = "shell", .members = NULL, .n_members = 0};
static const class_desc_t storage_class_desc = {.name = "storage", .members = NULL, .n_members = 0};

//
// Registers the introspection-and-utility subset of proposal §5.10's
// root methods: `objects`, `attributes`, `methods`, `help`, `print`,
// and `time`. These are the ones with no dependency on legacy command
// internals — wrappers for `cp`, `peeler`, `hd_*`, `rom_*`, `vrom_*`,
// `partmap`, `probe`, `list_partitions`, `unmount`, `let`, `quit`,
// `source`, `hd_create`, `hd_download` defer to a follow-up
// substrate-and-shell sub-commit (the `quit` / `source` ones in
// particular need shell-state plumbing).
//
// All five introspection methods accept an optional path string; an
// empty / missing path resolves to the root itself.

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
// `rtc.time =` (M7b) instead.
static value_t method_root_time(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    return val_uint(8, (uint64_t)time(NULL));
}

// `print(value)` — formats a value as a string. The implementation
// just round-trips through V_STRING for now: numerics → decimal/hex
// per flags, strings stay strings, others get a class-shaped tag.
// This matches the proposal's §5.10 listing without committing to a
// rich formatter (which lands with M9 / M10).
static value_t method_root_print(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_str("");
    const value_t *v = &argv[0];
    char buf[256];
    switch (v->kind) {
    case V_NONE:
        return val_str("");
    case V_BOOL:
        return val_str(v->b ? "true" : "false");
    case V_INT:
        snprintf(buf, sizeof(buf), "%lld", (long long)v->i);
        return val_str(buf);
    case V_UINT:
        if (v->flags & VAL_HEX)
            snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v->u);
        else
            snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v->u);
        return val_str(buf);
    case V_FLOAT:
        snprintf(buf, sizeof(buf), "%g", v->f);
        return val_str(buf);
    case V_STRING:
        return val_str(v->s ? v->s : "");
    case V_ENUM:
        if (v->enm.table && (size_t)v->enm.idx < v->enm.n_table && v->enm.table[v->enm.idx])
            return val_str(v->enm.table[v->enm.idx]);
        snprintf(buf, sizeof(buf), "<enum:%d>", v->enm.idx);
        return val_str(buf);
    case V_OBJECT: {
        const class_desc_t *cls = v->obj ? object_class(v->obj) : NULL;
        snprintf(buf, sizeof(buf), "<object:%s>", cls && cls->name ? cls->name : "?");
        return val_str(buf);
    }
    default:
        return val_str("<value>");
    }
}

// --- M8 (slice 4) — root methods that wrap legacy shell commands ----------
//
// Per proposal §5.10 these are top-level methods that flatten existing
// `image foo` / `hd foo` / `rom foo` / `vrom foo` / `peeler` / `cp` /
// `quit` shell forms into one-call methods. Each wrapper now calls
// the underlying C primitive (or `shell_<cmd>_argv` for the rich
// parsers) directly — phase 5b retired the shell_dispatch round-trip.
//
// `let` and `source` defer to a later sub-commit — they touch
// shell-state plumbing (variables, script context) that the M9–M10
// cutover rewrites end-to-end.
//
// `hd_download` defers similarly — it requires platform/network state
// outside the scope of the object tree.

// `cp([-r], src, dst)` — top-level alias for `storage.import` per
// proposal §5.10 ("preserve UNIX muscle memory"). Accepts `-r` / `-R`
// in any positional slot to match POSIX muscle memory.
static value_t method_root_cp(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    bool recursive = false;
    const char *src = NULL;
    const char *dst = NULL;
    for (int i = 0; i < argc; i++) {
        if (argv[i].kind != V_STRING || !argv[i].s)
            return val_err("cp: expected ([-r], src, dst)");
        const char *s = argv[i].s;
        if (strcmp(s, "-r") == 0 || strcmp(s, "-R") == 0) {
            recursive = true;
        } else if (!src) {
            src = s;
        } else if (!dst) {
            dst = s;
        } else {
            return val_err("cp: too many arguments");
        }
    }
    if (!src || !dst)
        return val_err("cp: expected ([-r], src, dst)");
    char err[256] = {0};
    int rc = shell_cp(src, dst, recursive, err, sizeof(err));
    if (rc < 0)
        return val_err("%s", err[0] ? err : "cp: failed");
    return val_bool(true);
}

// `peeler(path, [out_dir])` — extract a Mac archive
// (.sit/.cpt/.hqx/.bin) via the legacy `peeler` shell command.
// Returns true on successful extraction. Note: legacy peeler takes the
// output directory via `-o <dir>` (not as a positional arg), so this
// wrapper builds the line by hand rather than going through
// dispatch_with_string_args.
static value_t method_root_peeler(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("peeler: expected (path, [out_dir])");
    const char *path = argv[0].s ? argv[0].s : "";
    const char *out_dir = (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s) ? argv[1].s : NULL;
    return val_bool(peeler_shell_extract(path, out_dir) == 0);
}

// `peeler_probe(path)` — true if the given file is a peeler-supported
// archive (`peeler --probe` returns 0 on success).
static value_t method_root_peeler_probe(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("peeler_probe: expected (path)");
    const char *path = argv[0].s ? argv[0].s : "";
    peel_err_t *err = NULL;
    peel_buf_t buf = peel_read_file(path, &err);
    if (err) {
        fprintf(stderr, "peeler: cannot open '%s': %s\n", path, peel_err_msg(err));
        peel_err_free(err);
        return val_bool(false);
    }
    const char *format = peel_detect(buf.data, buf.size);
    if (format)
        printf("%s: Supported (%s format detected)\n", path, format);
    else
        printf("%s: NOT a supported format\n", path);
    peel_free(&buf);
    return val_bool(format != NULL);
}

// `fd_probe(path)` — true if the file is a recognised floppy image.
static value_t method_root_fd_probe(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("fd_probe: expected (path)");
    return val_bool(system_probe_floppy(argv[0].s ? argv[0].s : "") == 0);
}

// `find_media(dir, [dst])` — search a directory for a recognised
// floppy image; if `dst` is given, the image is copied there. Returns
// true if a match was found.
static value_t method_root_find_media(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("find_media: expected (dir, [dst])");
    const char *dir = argv[0].s ? argv[0].s : "";
    const char *dst = (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s) ? argv[1].s : NULL;
    return val_bool(gs_find_media(dir, dst) == 0);
}

// `hd_create(path, size)` — wraps `hd create <path> <size>`.
static value_t method_root_hd_create(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING)
        return val_err("hd_create: expected (path, size)");
    char line[512];
    int n;
    // size accepts either a string ("HD20SC", "40M", "21411840") or a
    // bare integer (raw byte count). The shell-form size parser handles
    // both, so just stringify whichever variant we got.
    if (argv[1].kind == V_STRING) {
        n = snprintf(line, sizeof(line), "hd create \"%s\" \"%s\"", argv[0].s, argv[1].s);
    } else if (argv[1].kind == V_INT) {
        n = snprintf(line, sizeof(line), "hd create \"%s\" %lld", argv[0].s, (long long)argv[1].i);
    } else if (argv[1].kind == V_UINT) {
        n = snprintf(line, sizeof(line), "hd create \"%s\" %llu", argv[0].s, (unsigned long long)argv[1].u);
    } else {
        return val_err("hd_create: size must be string or integer");
    }
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("hd_create: arguments too long");
    char *targv[32];
    int targc = tokenize(line, targv, 32);
    if (targc <= 0)
        return val_err("hd_create: tokenisation failed");
    return val_bool(shell_hd_argv(targc, targv) == 0);
}

// `hd_download(src, dst)` — export a hard disk image (base + delta) to a
// flat file. Wraps the legacy `hd download` command, which the headless
// build does support (the WASM platform's fetch glue is a separate
// concern that the typed wrapper doesn't change).
static value_t method_root_hd_download(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING || argv[1].kind != V_STRING)
        return val_err("hd_download: expected (source_path, dest_path)");
    int rc = system_download_hd(argv[0].s ? argv[0].s : "", argv[1].s ? argv[1].s : "");
    return val_bool(rc == 0);
}

// rom_* / vrom_* — wrap the `rom probe|validate` / `vrom probe|validate`
// subcommand forms. Each takes a single path argument. Two helpers
// codify the two legacy return conventions: cmd_int (0 = success) for
// the *_probe family and cmd_bool (1 = valid) for the *_validate family.
// `which` is 0 for rom, 1 for vrom.
static int64_t call_rom_subcmd(int which, const char *sub, int argc, const value_t *argv) {
    if (argc < 1 || argv[0].kind != V_STRING)
        return -2;
    const char *cmd = which ? "vrom" : "rom";
    char *fake_argv[] = {(char *)cmd, (char *)sub, (char *)(argv[0].s ? argv[0].s : "")};
    return (int64_t)(which ? cmd_vrom(3, fake_argv) : cmd_rom(3, fake_argv));
}

static value_t method_root_rom_probe(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    // No-arg form: probe the currently loaded ROM.
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s || !*argv[0].s) {
        char *fake_argv[] = {"rom", "probe"};
        return val_bool(cmd_rom_probe(2, fake_argv, 2) == 0);
    }
    char *fake_argv[] = {"rom", "probe", (char *)argv[0].s};
    return val_bool(cmd_rom_probe(3, fake_argv, 2) == 0);
}
static value_t method_root_rom_validate(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t rc = call_rom_subcmd(0, "validate", argc, argv);
    if (rc == -2)
        return val_err("rom_validate: expected (path)");
    // cmd_bool semantics: 1 = valid.
    return val_bool(rc == 1);
}
static value_t method_root_vrom_probe(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("vrom_probe: expected (path)");
    char *fake_argv[] = {"vrom", "probe", (char *)(argv[0].s ? argv[0].s : "")};
    return val_bool(cmd_vrom(3, fake_argv) == 0);
}
static value_t method_root_vrom_validate(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t rc = call_rom_subcmd(1, "validate", argc, argv);
    if (rc == -2)
        return val_err("vrom_validate: expected (path)");
    return val_bool(rc == 1);
}

// image_* — wraps the `image partmap|probe|list|unmount` subcommands
// per proposal §5.10's "Legacy `image foo` subcommand machinery
// becomes shims that flatten to top-level method calls". These four
// commands print info to stdout and return cmd_int; we expose the
// success bit so callers can branch on it.
static value_t image_subcmd_bool(const char *sub, int argc, const value_t *argv, const char *err_prefix) {
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("%s: expected (path)", err_prefix);
    char *fake_argv[] = {"image", (char *)sub, (char *)(argv[0].s ? argv[0].s : "")};
    return val_bool(shell_image_argv(3, fake_argv) == 0);
}
static const char *apm_fs_kind_label(enum apm_fs_kind k) {
    switch (k) {
    case APM_FS_HFS:
        return "HFS";
    case APM_FS_UFS:
        return "UFS";
    case APM_FS_PARTITION_MAP:
        return "map";
    case APM_FS_DRIVER:
        return "drvr";
    case APM_FS_FREE:
        return "free";
    case APM_FS_PATCHES:
        return "patch";
    default:
        return "--";
    }
}
static value_t method_root_partmap(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("partmap: expected (path)");
    const char *path = argv[0].s ? argv[0].s : "";
    image_t *img = image_open_readonly(path);
    if (!img)
        return val_err("partmap: cannot open image '%s'", path);
    const char *errmsg = NULL;
    apm_table_t *table = image_apm_parse(img, &errmsg);
    if (!table) {
        image_close(img);
        return val_err("partmap: not an APM image: %s", errmsg ? errmsg : "unknown error");
    }
    printf("format: APM (512B blocks, %zu total)\n", disk_size(img) / 512);
    printf("  #  Name                             Type                        Start        Size  FS\n");
    for (uint32_t i = 0; i < table->n_partitions; i++) {
        const apm_partition_t *p = &table->partitions[i];
        printf("  %-2u %-32s %-24s %10llu  %10llu  %s\n", (unsigned)p->index, p->name[0] ? p->name : "(unnamed)",
               p->type[0] ? p->type : "(unknown)", (unsigned long long)p->start_block,
               (unsigned long long)p->size_blocks, apm_fs_kind_label(p->fs_kind));
    }
    image_apm_free(table);
    image_close(img);
    return val_bool(true);
}
static value_t method_root_probe(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("probe: expected (path)");
    const char *path = argv[0].s ? argv[0].s : "";
    image_t *img = image_open_readonly(path);
    if (!img) {
        printf("cannot open image '%s'\n", path);
        return val_bool(false);
    }
    size_t size = disk_size(img);
    // APM first (matches the proposal's probe order — APM wins over ISO 9660
    // because Apple install CDs carry both).
    uint8_t block[512];
    bool apm = false;
    if (size >= 1024 && disk_read_data(img, 512, block, sizeof(block)) == sizeof(block))
        apm = image_apm_probe_magic(block);
    bool iso = false;
    if (size >= 33280 && disk_read_data(img, 32768, block, sizeof(block)) == sizeof(block))
        iso = (memcmp(block + 1, "CD001", 5) == 0);
    bool hfs = false;
    if (!apm && size >= 1024 + 512 && disk_read_data(img, 1024, block, sizeof(block)) == sizeof(block))
        hfs = (block[0] == 0x42 && block[1] == 0x44);
    if (apm && iso)
        printf("format: APM + ISO 9660 hybrid (%zu bytes)\n", size);
    else if (apm)
        printf("format: APM (%zu bytes)\n", size);
    else if (iso)
        printf("format: ISO 9660 (%zu bytes)\n", size);
    else if (hfs)
        printf("format: HFS (bare, %zu bytes)\n", size);
    else
        printf("format: unrecognised / raw (%zu bytes)\n", size);
    image_close(img);
    return val_bool(true);
}
// list_partitions / image_mounts both want the cached-mounts table via
// image_vfs_list. Render-row callbacks receive the per-image columns.
static void list_row_print(const char *path, const char *fmt, uint32_t n_parts, uint32_t refs, bool conflicted,
                           void *user) {
    bool *header_printed = (bool *)user;
    if (!*header_printed) {
        printf("PATH                                        FMT  PARTS  REFS  STATUS\n");
        *header_printed = true;
    }
    printf("%-44s %-3s %5u %5u  %s\n", path, fmt, n_parts, refs, conflicted ? "busy" : "ok");
}
static value_t method_root_list_partitions(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    bool header_printed = false;
    image_vfs_list(list_row_print, &header_printed);
    if (!header_printed)
        printf("(no cached image mounts)\n");
    return val_bool(true);
}
static value_t method_root_unmount(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("unmount: expected (path)");
    const char *path = argv[0].s ? argv[0].s : "";
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *bctx = NULL;
    const char *tail = NULL;
    if (vfs_resolve(path, resolved, sizeof(resolved), &be, &bctx, &tail) == 0)
        path = resolved;
    int rc = image_vfs_unmount(path);
    if (rc == 0) {
        printf("unmounted %s\n", path);
        return val_bool(true);
    }
    if (rc == -ENOENT)
        printf("image unmount: not currently mounted: %s\n", path);
    else if (rc == -EBUSY)
        printf("image unmount: %s has live handles; marked conflicted\n", path);
    else
        printf("image unmount: %s: %s\n", path, strerror(-rc));
    return val_bool(false);
}

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

// === Checkpoint root methods (M10b — checkpoint area) ======================
//
// Thin V_BOOL wrappers around the legacy `checkpoint <subcmd>` form.
// `running()` exposes scheduler_is_running so JS can replace
// `runCommand('status') === 1` with a direct boolean read.

static value_t method_root_checkpoint_probe(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    return val_bool(find_valid_checkpoint_path() != NULL);
}

static value_t method_root_checkpoint_clear(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    return val_bool(gs_checkpoint_clear() == 0);
}

static value_t method_root_checkpoint_load(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc >= 1 && argv[0].kind == V_STRING && argv[0].s && *argv[0].s) {
        char *fake_argv[2] = {"--load", (char *)argv[0].s};
        return val_bool(cmd_load_checkpoint(2, fake_argv) == 0);
    }
    char *fake_argv[1] = {"--load"};
    return val_bool(cmd_load_checkpoint(1, fake_argv) == 0);
}

static value_t method_root_checkpoint_save(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("checkpoint_save: expected (path, [mode])");
    const char *path = argv[0].s ? argv[0].s : "";
    char *fake_argv[3] = {"--save", (char *)path, NULL};
    int fake_argc = 2;
    if (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s) {
        fake_argv[2] = (char *)argv[1].s;
        fake_argc = 3;
    }
    return val_bool(cmd_save_checkpoint(fake_argc, fake_argv) == 0);
}

static value_t method_root_register_machine(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING || argv[1].kind != V_STRING)
        return val_err("register_machine: expected (id, created)");
    return val_bool(gs_register_machine(argv[0].s ? argv[0].s : "", argv[1].s ? argv[1].s : "") == 0);
}

// `auto_checkpoint` attribute (V_BOOL, rw) — exposes the WASM
// background-checkpoint loop's enabled flag. Headless's weak defaults
// stub out (no auto-checkpoint there), so reads return false and
// writes are silently ignored on that platform.
static value_t attr_auto_checkpoint_get(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_bool(gs_checkpoint_auto_get());
}

static value_t attr_auto_checkpoint_set(struct object *self, const member_t *m, value_t in) {
    (void)self;
    (void)m;
    bool b = val_as_bool(&in);
    value_free(&in);
    gs_checkpoint_auto_set(b);
    return val_none();
}

// `running()` — true if the scheduler is currently running. Mirrors
// the legacy `status` command's int return (cmd_int 1 = running).
static value_t method_root_running(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    scheduler_t *s = system_scheduler();
    return val_bool(s ? scheduler_is_running(s) : false);
}

// === ROM / disk-mount / scheduler root methods (M10b — drop area) ==========

// `rom_checksum(path)` — return the 8-char hex checksum of a ROM file,
// or empty string when the file doesn't validate. Mirrors the legacy
// `rom checksum` command's printed output as a typed return value, so
// JS can avoid runCommandJSON + stdout-parsing.
static value_t method_root_rom_checksum(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("rom_checksum: expected (path)");
    FILE *f = fopen(argv[0].s, "rb");
    if (!f)
        return val_str("");
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return val_str("");
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > 2 * 1024 * 1024) {
        fclose(f);
        return val_str("");
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return val_str("");
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return val_str("");
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return val_str("");
    }
    uint32_t cksum = 0;
    const rom_info_t *info = rom_identify_data(buf, (size_t)sz, &cksum);
    free(buf);
    if (!info)
        return val_str("");
    char hex[16];
    snprintf(hex, sizeof(hex), "%08X", cksum);
    return val_str(hex);
}

static value_t method_root_rom_load(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("rom_load: expected (path)");
    return val_bool(cmd_rom_load(argv[0].s ? argv[0].s : "") == 0);
}

// `fd_insert(path, slot, writable)` — mount a floppy image into one of
// the 1–2 floppy drives. Returns true on successful insert.
static value_t method_root_fd_insert(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING)
        return val_err("fd_insert: expected (path, slot, [writable])");
    int64_t slot = 0;
    bool ok = false;
    slot = (int64_t)val_as_i64(&argv[1], &ok);
    if (!ok && argv[1].kind == V_UINT)
        slot = (int64_t)argv[1].u;
    bool writable = false;
    if (argc >= 3)
        writable = val_as_bool(&argv[2]);
    char line[1024];
    int n = snprintf(line, sizeof(line), "fd insert \"%s\" %lld %s", argv[0].s ? argv[0].s : "", (long long)slot,
                     writable ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("fd_insert: arguments too long");
    char *targv[32];
    int targc = tokenize(line, targv, 32);
    if (targc <= 0)
        return val_err("fd_insert: tokenisation failed");
    return val_bool(shell_fd_argv(targc, targv) == 0);
}

// `run([cycles])` — start the scheduler. With no argument, runs
// indefinitely (until paused / exception). With a cycle count, runs
// for that many CPU cycles and pauses.
static value_t method_root_run(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    // Guard against being called before any machine is loaded (e.g. the
    // basic-ui `?noui` smoke test that pings runCommand before booting).
    // The legacy `cmd_run` GS_ASSERT prints + returns rather than aborting,
    // then dereferences NULL on s->mode — under the gsEval path the worker
    // is fully initialised so the crash is reachable; under the older
    // executeShellCommand path the cmd registry happened to lose this race.
    if (!system_scheduler())
        return val_err("run: no machine loaded");
    char cycles_buf[32];
    if (argc >= 1) {
        bool ok = false;
        int64_t cycles = (int64_t)val_as_i64(&argv[0], &ok);
        if (!ok && argv[0].kind == V_UINT)
            cycles = (int64_t)argv[0].u;
        snprintf(cycles_buf, sizeof(cycles_buf), "%lld", (long long)cycles);
        char *fake_argv[] = {"run", cycles_buf};
        return val_bool(cmd_run(2, fake_argv) == 0);
    }
    char *fake_argv[] = {"run"};
    return val_bool(cmd_run(1, fake_argv) == 0);
}

// === Boot/setup root methods (M10b — url-media + config-dialog area) =======

// `vrom_load(path)` — set the video-ROM path for the next machine init.
static value_t method_root_vrom_load(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("vrom_load: expected (path)");
    char *fake_argv[] = {"vrom", "load", (char *)(argv[0].s ? argv[0].s : "")};
    return val_bool(cmd_vrom(3, fake_argv) == 0);
}

// `hd_attach(path, id)` — attach a hard-disk image at the given SCSI id.
static value_t method_root_hd_attach(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("hd_attach: expected (path, [id])");
    int64_t id = 0; // Legacy default — matches cmd_hd_handler's `attach` branch.
    if (argc >= 2) {
        bool ok = false;
        id = (int64_t)val_as_i64(&argv[1], &ok);
        if (!ok && argv[1].kind == V_UINT)
            id = (int64_t)argv[1].u;
    }
    char line[1024];
    int n = snprintf(line, sizeof(line), "hd attach \"%s\" %lld", argv[0].s ? argv[0].s : "", (long long)id);
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("hd_attach: arguments too long");
    char *targv[32];
    int targc = tokenize(line, targv, 32);
    if (targc <= 0)
        return val_err("hd_attach: tokenisation failed");
    return val_bool(shell_hd_argv(targc, targv) == 0);
}

// === Debugging-area root methods ===========================================
//
// Thin wrappers around the legacy `info` / `d` / `break` / `logpoint` /
// `log` shell commands. The legacy parsing stays in their respective
// handlers; these methods exist so test scripts and the typed-bridge
// have one consistent path-form interface.

static value_t method_root_disasm(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t count = 16;
    if (argc >= 1) {
        bool ok = false;
        count = val_as_i64(&argv[0], &ok);
        if (!ok)
            return val_err("disasm: count must be integer");
        if (count <= 0)
            count = 16;
    }
    cpu_t *cpu = system_cpu();
    if (!cpu)
        return val_err("disasm: CPU not initialised");
    uint32_t addr = cpu_get_pc(cpu);
    char buf[160];
    for (int i = 0; i < (int)count; i++) {
        int instr_len = debugger_disasm(buf, sizeof(buf), addr);
        printf("%s\n", buf);
        addr += 2 * instr_len;
    }
    return val_bool(true);
}

static value_t method_root_break_list_dump(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    debug_t *debug = system_debug();
    if (!debug)
        return val_err("break_list_dump: debug not available");
    list_breakpoints(debug);
    return val_bool(true);
}

static value_t method_root_break_clear(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    debug_t *debug = system_debug();
    if (!debug)
        return val_err("break_clear: debug not available");
    int count = delete_all_breakpoints(debug);
    printf("Deleted %d breakpoint(s).\n", count);
    return val_bool(true);
}

static value_t method_root_logpoint_list_dump(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    debug_t *debug = system_debug();
    if (!debug)
        return val_err("logpoint_list_dump: debug not available");
    list_logpoints(debug);
    return val_bool(true);
}

static value_t method_root_logpoint_clear(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    debug_t *debug = system_debug();
    if (!debug)
        return val_err("logpoint_clear: debug not available");
    delete_all_logpoints(debug);
    return val_bool(true);
}

// `logpoint_set(spec)` — pass the legacy logpoint spec as a single string
// (e.g. `--write 0x000016A.l "Ticks bumped..." level=5`). Calls the legacy
// `cmd_logpoint_handler` parser via shell_logpoint_argv (no shell_dispatch).
static value_t method_root_logpoint_set(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("logpoint_set: expected (spec_string)");
    char line[2048];
    int n = snprintf(line, sizeof(line), "logpoint %s", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("logpoint_set: argument too long");
    char *targv[32];
    int targc = tokenize(line, targv, 32);
    if (targc <= 0)
        return val_err("logpoint_set: empty spec");
    return val_bool(shell_logpoint_argv(targc, targv) == 0);
}

// `log_set(subsys, level_or_spec)` — adjust per-subsystem log level. The
// second arg accepts either an integer level or a full named-arg spec
// string (e.g. `"level=5 file=/tmp/foo.txt stdout=off ts=on"`); spec
// strings are tokenised and forwarded to cmd_log directly (no
// shell_dispatch).
static value_t method_root_log_set(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING)
        return val_err("log_set: expected (subsys, level|spec)");
    char line[512];
    int n;
    if (argv[1].kind == V_STRING) {
        n = snprintf(line, sizeof(line), "log %s %s", argv[0].s, argv[1].s ? argv[1].s : "");
    } else {
        bool ok = false;
        int64_t level = val_as_i64(&argv[1], &ok);
        if (!ok)
            return val_err("log_set: second arg must be integer level or spec string");
        n = snprintf(line, sizeof(line), "log %s %lld", argv[0].s, (long long)level);
    }
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("log_set: argument too long");
    char *targv[32];
    int targc = tokenize(line, targv, 32);
    if (targc <= 0)
        return val_err("log_set: empty spec");
    return val_bool(cmd_log(targc, targv) == 0);
}

// `stop()` — interrupt the scheduler.
static value_t method_root_stop(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    scheduler_t *s = system_scheduler();
    if (!s)
        return val_err("stop: scheduler not initialised");
    scheduler_stop(s);
    return val_bool(true);
}

// `step([n])` — single-step n instructions (default 1).
static value_t method_root_step(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t count = 1;
    if (argc >= 1) {
        bool ok = false;
        count = val_as_i64(&argv[0], &ok);
        if (!ok)
            return val_err("step: count must be integer");
    }
    if (count <= 0)
        return val_err("step: count must be positive");
    scheduler_t *s = system_scheduler();
    if (!s)
        return val_err("step: scheduler not initialised");
    scheduler_run_instructions(s, (int)count);
    scheduler_stop(s);
    return val_bool(true);
}

// `background_checkpoint(name)` — capture a snapshot under the given label.
// Routes to the platform-specific gs_background_checkpoint (WASM
// implements via save_quick_checkpoint; headless prints a "not
// supported" stub).
static value_t method_root_background_checkpoint(struct object *self, const member_t *m, int argc,
                                                 const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("background_checkpoint: expected (name)");
    return val_bool(gs_background_checkpoint(argv[0].s ? argv[0].s : "") == 0);
}

// `path_exists(path)` — true if the path exists in the shell VFS.
static value_t method_root_path_exists(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("path_exists: expected (path)");
    vfs_stat_t st;
    return val_bool(vfs_stat(argv[0].s ? argv[0].s : "", &st) == 0);
}

// `path_size(path)` — file size in bytes (0 on stat failure).
static value_t method_root_path_size(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("path_size: expected (path)");
    const char *path = argv[0].s ? argv[0].s : "";
    vfs_stat_t st = {0};
    int rc = vfs_stat(path, &st);
    if (rc < 0) {
        printf("size: cannot stat '%s': %s\n", path, strerror(-rc));
        return val_uint(8, 0);
    }
    return val_uint(8, st.size);
}

// `fd_create(path, [drive_or_hd])` — create a blank 800 KB floppy image
// and auto-mount it. The optional second arg matches the legacy
// `fd create [--hd] <path> [drive]` parser: pass the string `"--hd"`
// for a 1.44 MB image, or an integer 0/1 to target a specific drive.
// String integers (e.g. `"0"`) are accepted too — that's how the
// integration scripts spell it.
static value_t method_root_fd_create(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("fd_create: expected (path, [drive_or_hd])");
    bool high_density = false;
    int preferred = -1;
    if (argc >= 2) {
        if (argv[1].kind == V_STRING && argv[1].s) {
            if (strcmp(argv[1].s, "--hd") == 0) {
                high_density = true;
            } else if (argv[1].s[0] >= '0' && argv[1].s[0] <= '1' && argv[1].s[1] == '\0') {
                preferred = argv[1].s[0] - '0';
            } else if (*argv[1].s) {
                return val_err("fd_create: second arg must be \"--hd\" or drive index 0/1");
            }
        } else if (argv[1].kind == V_INT || argv[1].kind == V_UINT) {
            int64_t d = (argv[1].kind == V_INT) ? argv[1].i : (int64_t)argv[1].u;
            if (d != 0 && d != 1)
                return val_err("fd_create: drive index must be 0 or 1");
            preferred = (int)d;
        }
    }
    int rc = system_create_floppy(argv[0].s ? argv[0].s : "", high_density, preferred);
    return val_bool(rc == 0);
}

// `print_value(target)` — read a register / condition code / memory cell
// by calling the legacy `print` handler via shell_print_argv (no
// shell_dispatch). Returns the numeric value as V_UINT; the test
// convention uses `>>> 0` to truncate to uint32.
static value_t method_root_print_value(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("print_value: expected (target)");
    char line[256];
    int n = snprintf(line, sizeof(line), "print %s", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("print_value: target too long");
    char *targv[32];
    int targc = tokenize(line, targv, 32);
    if (targc <= 0)
        return val_err("print_value: empty target");
    return val_uint(8, (uint64_t)shell_print_argv(targc, targv));
}

// `set_value(target, value)` — write a register / condition code / memory
// cell via the legacy `set` command. Target syntax matches the legacy
// command (`d5`, `pc`, `z`, `0x1000.b`, etc.). Calls cmd_set directly
// (no shell_dispatch).
static value_t method_root_set_value(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING)
        return val_err("set_value: expected (target, value)");
    char value_buf[64];
    const char *value_str = NULL;
    if (argv[1].kind == V_STRING) {
        value_str = argv[1].s ? argv[1].s : "";
    } else {
        bool ok = false;
        uint64_t v = val_as_u64(&argv[1], &ok);
        if (!ok)
            return val_err("set_value: value must be integer or string");
        snprintf(value_buf, sizeof(value_buf), "0x%llx", (unsigned long long)v);
        value_str = value_buf;
    }
    char line[256];
    int n = snprintf(line, sizeof(line), "set %s %s", argv[0].s ? argv[0].s : "", value_str);
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("set_value: argument too long");
    char *targv[32];
    int targc = tokenize(line, targv, 32);
    if (targc <= 0)
        return val_err("set_value: tokenisation failed");
    return val_bool(cmd_set(targc, targv) == 0);
}

// `examine(addr, [count])` — hex-dump `count` bytes from `addr` (legacy
// `x` / `examine`). `addr` accepts integer or string (alias / expression).
static value_t method_root_examine(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("examine: expected (addr, [count])");
    char line[128];
    int n;
    bool addr_ok = false;
    uint64_t addr_u = val_as_u64(&argv[0], &addr_ok);
    bool addr_is_str = (argv[0].kind == V_STRING);
    if (!addr_ok && !addr_is_str)
        return val_err("examine: addr must be integer or string");
    int64_t count = 0;
    bool have_count = false;
    if (argc >= 2) {
        bool ok = false;
        count = val_as_i64(&argv[1], &ok);
        if (!ok)
            return val_err("examine: count must be integer");
        have_count = true;
    }
    if (have_count) {
        if (addr_ok)
            n = snprintf(line, sizeof(line), "x 0x%llx %lld", (unsigned long long)addr_u, (long long)count);
        else
            n = snprintf(line, sizeof(line), "x %s %lld", argv[0].s ? argv[0].s : "", (long long)count);
    } else {
        if (addr_ok)
            n = snprintf(line, sizeof(line), "x 0x%llx", (unsigned long long)addr_u);
        else
            n = snprintf(line, sizeof(line), "x %s", argv[0].s ? argv[0].s : "");
    }
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("examine: argument too long");
    char *targv[32];
    int targc = tokenize(line, targv, 32);
    if (targc <= 0)
        return val_err("examine: tokenisation failed");
    return val_bool(shell_examine_argv(targc, targv) == 0);
}

// `hd_models()` — return the known SCSI HD model catalog as a single
// JSON-encoded V_STRING (array of `{label, vendor, product, size}`).
// The web frontend's "create disk" dialog reads this list to populate
// its drive picker; returning the JSON inline retires the last
// `runCommandJSON("hd models --json")` caller in app/web/js.
static value_t method_root_hd_models(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    int count = drive_catalog_count();
    size_t cap = 64 + (size_t)count * 128;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return val_err("hd_models: out of memory");
    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, cap - pos, "[");
    for (int i = 0; i < count && pos + 256 < cap; i++) {
        const struct drive_model *md = drive_catalog_get(i);
        if (!md)
            continue;
        pos += (size_t)snprintf(buf + pos, cap - pos,
                                "%s{\"label\":\"%s\",\"vendor\":\"%s\",\"product\":\"%s\",\"size\":%zu}", i ? "," : "",
                                md->label, md->vendor, md->product, md->size);
    }
    if (pos + 1 < cap)
        pos += (size_t)snprintf(buf + pos, cap - pos, "]");
    value_t v = val_str(buf);
    free(buf);
    return v;
}

// `cdrom_attach(path)` — attach a CD-ROM image to the system.
static value_t method_root_cdrom_attach(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("cdrom_attach: expected (path)");
    if (!global_emulator)
        return val_err("cdrom_attach: emulator not initialised");
    add_scsi_cdrom(global_emulator, argv[0].s ? argv[0].s : "", 3);
    return val_bool(true);
}

// `hd_validate(path)` — true if the file passes legacy `hd validate`
// (cmd_bool 1 = valid).
static value_t method_root_hd_validate(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("hd_validate: expected (path)");
    const char *path = argv[0].s ? argv[0].s : "";
    image_t *img = image_open_readonly(path);
    if (!img) {
        printf("invalid SCSI HD image: cannot open %s\n", path);
        return val_bool(false);
    }
    if (img->type == image_fd_ss || img->type == image_fd_ds || img->type == image_fd_hd) {
        printf("invalid SCSI HD image: size matches floppy (%zu bytes), use fd validate\n", img->raw_size);
        image_close(img);
        return val_bool(false);
    }
    size_t sz = img->raw_size;
    const struct drive_model *best = drive_catalog_find_closest(sz);
    if (sz == best->size)
        printf("valid SCSI HD image: %zu bytes, matches %s %s\n", sz, best->vendor, best->product);
    else
        printf("valid SCSI HD image: %zu bytes, nearest model %s %s\n", sz, best->vendor, best->product);
    image_close(img);
    return val_bool(true);
}

// `cdrom_validate(path)` — true if the file is a recognised CD-ROM image
// (ISO 9660, HFS, or Apple Partition Map). Mirrors the legacy `cdrom
// validate` body (system.c:1003) but works against the public disk_*
// API so no shell_dispatch round-trip is required.
static value_t method_root_cdrom_validate(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("cdrom_validate: expected (path)");
    const char *path = argv[0].s ? argv[0].s : "";
    image_t *img = image_open_readonly(path);
    if (!img) {
        printf("invalid CD-ROM image: cannot open %s\n", path);
        return val_bool(false);
    }
    if (img->type == image_fd_ss || img->type == image_fd_ds || img->type == image_fd_hd) {
        printf("invalid CD-ROM image: floppy-sized (%zu bytes)\n", img->raw_size);
        image_close(img);
        return val_bool(false);
    }
    bool is_iso = false, is_hfs = false, is_apm = false;
    size_t sz = disk_size(img);
    uint8_t sector[512];
    // ISO 9660: "CD001" at offset 32769 = sector 64, byte 1
    if (sz >= 33280) {
        disk_read_data(img, 32768, sector, 512);
        if (memcmp(sector + 1, "CD001", 5) == 0)
            is_iso = true;
    }
    // HFS: 0x4244 at offset 1024 = sector 2, byte 0
    if (sz >= 1536) {
        disk_read_data(img, 1024, sector, 512);
        if (sector[0] == 0x42 && sector[1] == 0x44)
            is_hfs = true;
    }
    // Apple Partition Map: DDM 0x4552 at sector 0 + PM 0x504D at sector 1
    if (sz >= 1024) {
        disk_read_data(img, 0, sector, 512);
        bool has_ddm = (sector[0] == 0x45 && sector[1] == 0x52);
        disk_read_data(img, 512, sector, 512);
        bool has_pm = (sector[0] == 0x50 && sector[1] == 0x4D);
        if (has_ddm && has_pm)
            is_apm = true;
    }
    double size_mb = (double)sz / (1024.0 * 1024.0);
    if (is_iso && is_hfs)
        printf("valid CD-ROM image: %.1f MB, ISO 9660 + HFS hybrid\n", size_mb);
    else if (is_iso)
        printf("valid CD-ROM image: %.1f MB, ISO 9660\n", size_mb);
    else if (is_hfs)
        printf("valid CD-ROM image: %.1f MB, HFS\n", size_mb);
    else if (is_apm)
        printf("valid CD-ROM image: %.1f MB, Apple Partition Map\n", size_mb);
    else {
        printf("invalid CD-ROM image: no ISO 9660, HFS, or Apple Partition Map detected\n");
        image_close(img);
        return val_bool(false);
    }
    image_close(img);
    return val_bool(true);
}

// `cdrom_eject(id)` — eject the CD-ROM at the given SCSI id (default 3).
// `scsi.devices[id].eject()` already exists for per-device ejection;
// this wrapper preserves the legacy `cdrom eject [id]` shape so the
// integration scripts have a 1:1 mapping.
static value_t method_root_cdrom_eject(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t id = 3;
    if (argc >= 1) {
        bool ok = false;
        id = (int64_t)val_as_i64(&argv[0], &ok);
        if (!ok && argv[0].kind == V_UINT)
            id = (int64_t)argv[0].u;
    }
    if (!global_emulator || !global_emulator->scsi)
        return val_err("cdrom_eject: emulator not initialised");
    int rc = scsi_eject_device(global_emulator->scsi, (int)id);
    if (rc < 0)
        return val_err("cdrom_eject: invalid SCSI ID %lld (expected 0..6)", (long long)id);
    if (rc == 0)
        printf("cdrom eject: no disc in SCSI ID %lld\n", (long long)id);
    else
        printf("cdrom eject: ejected disc from SCSI ID %lld\n", (long long)id);
    return val_bool(true);
}

// `cdrom_info(id)` — print info about the CD-ROM at the given SCSI id
// (default 3). Returns true if a disc is present, false if the slot is
// empty / wrong type. The detail lines are printed via the legacy
// command's printf; scripts can also walk `scsi.devices[id].*` directly
// for structured access.
static value_t method_root_cdrom_info(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t id = 3;
    if (argc >= 1) {
        bool ok = false;
        id = (int64_t)val_as_i64(&argv[0], &ok);
        if (!ok && argv[0].kind == V_UINT)
            id = (int64_t)argv[0].u;
    }
    if (!global_emulator || !global_emulator->scsi)
        return val_err("cdrom_info: emulator not initialised");
    if (id < 0 || id > 6)
        return val_err("cdrom_info: invalid SCSI ID %lld (expected 0..6)", (long long)id);
    int t = scsi_device_type(global_emulator->scsi, (unsigned)id);
    if (t != /* scsi_dev_cdrom */ 2) {
        printf("cdrom info: SCSI ID %lld is not a CD-ROM device\n", (long long)id);
        return val_bool(true);
    }
    image_t *img = scsi_device_image(global_emulator->scsi, (unsigned)id);
    if (!img) {
        printf("cdrom info: SCSI ID %lld — no disc\n", (long long)id);
        return val_bool(true);
    }
    const char *fname = image_get_filename(img);
    size_t sz = disk_size(img);
    double size_mb = (double)sz / (1024.0 * 1024.0);
    printf("cdrom info: SCSI ID %lld — %.1f MB — %s\n", (long long)id, size_mb, fname ? fname : "(unknown)");
    return val_bool(true);
}

// `image_mounts()` — list currently-mounted image paths (the legacy
// `image list` no-arg form). Same body as method_root_list_partitions
// since the underlying primitive (image_vfs_list) is the same.
static value_t method_root_image_mounts(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    bool header_printed = false;
    image_vfs_list(list_row_print, &header_printed);
    if (!header_printed)
        printf("(no cached image mounts)\n");
    return val_bool(true);
}

// `fd_validate(path)` — return the floppy density tag ("400K", "800K",
// "1.4MB", …) when the file is a recognised floppy image, or empty
// string otherwise. The legacy `fd validate` command prints the
// density to stdout and uses cmd_bool, so this wrapper opens the
// image directly via image_open_readonly to extract the type and
// avoid stdout-parsing.
static value_t method_root_fd_validate(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("fd_validate: expected (path)");
    image_t *img = image_open_readonly(argv[0].s ? argv[0].s : "");
    if (!img)
        return val_str("");
    const char *density = "";
    switch (img->type) {
    case image_fd_ss:
        density = "400K";
        break;
    case image_fd_ds:
        density = "800K";
        break;
    case image_fd_hd:
        density = "1.4MB";
        break;
    default:
        density = "";
        break;
    }
    image_close(img);
    return val_str(density);
}

// `setup_machine(model, ram_kb)` — run the legacy `setup --model X
// --ram Y` command that primes the next `rom load` for a specific
// machine profile. Renamed in the gsEval surface so the top-level
// namespace doesn't grow a generic `setup` verb.
static value_t method_root_setup_machine(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("setup_machine: expected (model, [ram_kb])");
    bool ok = false;
    int64_t ram = (argc >= 2) ? (int64_t)val_as_i64(&argv[1], &ok) : 0;
    if (!ok && argc >= 2 && argv[1].kind == V_UINT)
        ram = (int64_t)argv[1].u;
    char line[256];
    int n;
    if (argc >= 2)
        n = snprintf(line, sizeof(line), "setup --model %s --ram %lld", argv[0].s ? argv[0].s : "", (long long)ram);
    else
        n = snprintf(line, sizeof(line), "setup --model %s", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("setup_machine: arguments too long");
    char *targv[32];
    int targc = tokenize(line, targv, 32);
    if (targc <= 0)
        return val_err("setup_machine: tokenisation failed");
    return val_bool(cmd_setup(targc, targv) == 0);
}

// `schedule(mode)` — set the scheduler mode (max / realtime / hardware).
static value_t method_root_schedule(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("schedule: expected (mode)");
    const char *mode_str = argv[0].s ? argv[0].s : "";
    enum schedule_mode mode;
    if (strcmp(mode_str, "max") == 0)
        mode = schedule_max_speed;
    else if (strcmp(mode_str, "real") == 0)
        mode = schedule_real_time;
    else if (strcmp(mode_str, "hw") == 0)
        mode = schedule_hw_accuracy;
    else
        return val_err("schedule: unknown mode '%s' (valid: max, real, hw)", mode_str);
    scheduler_t *s = system_scheduler();
    if (!s)
        return val_err("schedule: scheduler not initialised");
    scheduler_set_mode(s, mode);
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

static const arg_decl_t root_cp_args[] = {
    {.name = "src", .kind = V_STRING, .doc = "Source path"},
    {.name = "dst", .kind = V_STRING, .doc = "Destination path"},
    {.name = "flags",
     .kind = V_STRING,
     .flags = OBJ_ARG_OPTIONAL,
     .doc = "Optional flags (e.g. -r for recursive copy)"},
};
static const arg_decl_t root_peeler_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Archive path"},
    {.name = "out_dir", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Optional extraction directory"},
};
static const arg_decl_t root_hd_create_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Output path"                          },
    {.name = "size", .kind = V_STRING, .doc = "Image size (e.g. \"40M\" or \"512K\")"},
};
static const arg_decl_t root_hd_download_args[] = {
    {.name = "src", .kind = V_STRING, .doc = "Source HD image path (base + delta)"},
    {.name = "dst", .kind = V_STRING, .doc = "Destination flat-file path"         },
};
static const arg_decl_t root_path_arg[] = {
    {.name = "path", .kind = V_STRING, .doc = "File path"},
};
static const arg_decl_t root_path_arg_optional[] = {
    {.name = "path",
     .kind = V_STRING,
     .flags = OBJ_ARG_OPTIONAL,
     .doc = "File path; empty falls back to the currently-loaded ROM"},
};
static const arg_decl_t root_hd_attach_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "HD image path"},
    {.name = "id", .kind = V_INT, .flags = OBJ_ARG_OPTIONAL, .doc = "SCSI bus index 0-6 (default 0)"},
};
static const arg_decl_t root_cdrom_id_arg[] = {
    {.name = "id", .kind = V_INT, .flags = OBJ_ARG_OPTIONAL, .doc = "SCSI id 0-6 (default 3)"},
};
static const arg_decl_t root_setup_machine_args[] = {
    {.name = "model", .kind = V_STRING, .doc = "Machine model id (plus / se30 / iicx)"},
    {.name = "ram_kb", .kind = V_UINT, .flags = OBJ_ARG_OPTIONAL, .doc = "RAM size in KB"},
};
static const arg_decl_t root_schedule_args[] = {
    {.name = "mode", .kind = V_STRING, .doc = "Scheduler mode (max | realtime | hardware)"},
};
static const arg_decl_t root_find_media_args[] = {
    {.name = "dir", .kind = V_STRING, .doc = "Directory to scan"},
    {.name = "dst", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Optional path to copy match into"},
};
static const arg_decl_t root_checkpoint_load_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .flags = OBJ_ARG_OPTIONAL,
     .doc = "Checkpoint path; empty auto-loads the latest"},
};
static const arg_decl_t root_checkpoint_save_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Checkpoint output path"},
    {.name = "mode", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Optional 'content' or 'refs' mode"},
};
static const arg_decl_t root_register_machine_args[] = {
    {.name = "id",      .kind = V_STRING, .doc = "Machine identity (UUID-like)"},
    {.name = "created", .kind = V_STRING, .doc = "Creation timestamp"          },
};
static const arg_decl_t root_fd_insert_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Floppy image path"},
    {.name = "slot", .kind = V_INT, .doc = "Drive index (0 = upper, 1 = lower)"},
    {.name = "writable", .kind = V_BOOL, .flags = OBJ_ARG_OPTIONAL, .doc = "Mount writable (default false)"},
};
static const arg_decl_t root_run_args[] = {
    {.name = "cycles", .kind = V_UINT, .flags = OBJ_ARG_OPTIONAL, .doc = "Optional cycle budget; 0 = run until paused"},
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
static const arg_decl_t root_print_args[] = {
    {.name = "value", .kind = V_NONE, .doc = "Value to format"},
};

static const member_t emu_root_members[] = {
    {.kind = M_METHOD,
     .name = "objects",
     .doc = "List child object names at the given path (or root)",
     .method = {.args = root_path_args, .nargs = 1, .result = V_LIST, .fn = method_root_objects}                     },
    {.kind = M_METHOD,
     .name = "attributes",
     .doc = "List attribute names of the resolved object's class",
     .method = {.args = root_path_args, .nargs = 1, .result = V_LIST, .fn = method_root_attributes}                  },
    {.kind = M_METHOD,
     .name = "methods",
     .doc = "List method names of the resolved object's class",
     .method = {.args = root_path_args, .nargs = 1, .result = V_LIST, .fn = method_root_methods}                     },
    {.kind = M_METHOD,
     .name = "help",
     .doc = "Return the doc string of a resolved member (or class name)",
     .method = {.args = root_help_args, .nargs = 1, .result = V_STRING, .fn = method_root_help}                      },
    {.kind = M_METHOD,
     .name = "time",
     .doc = "Wall-clock seconds since the Unix epoch",
     .method = {.args = NULL, .nargs = 0, .result = V_UINT, .fn = method_root_time}                                  },
    {.kind = M_METHOD,
     .name = "print",
     .doc = "Format a value as a string for display",
     .method = {.args = root_print_args, .nargs = 1, .result = V_STRING, .fn = method_root_print}                    },
    // Legacy-command wrappers (M8 slice 4 — proposal §5.10).
    // Side-effect wrappers return V_BOOL (true on dispatch success) so
    // the M10b migrators can branch on the result without re-deriving
    // the legacy command's int / cmd_bool convention.
    {.kind = M_METHOD,
     .name = "cp",
     .doc = "Copy a file or directory (alias for storage.import / legacy `cp`)",
     .method = {.args = root_cp_args, .nargs = 3, .result = V_BOOL, .fn = method_root_cp}                            },
    {.kind = M_METHOD,
     .name = "peeler",
     .doc = "Extract a Mac archive (.sit/.cpt/.hqx/.bin)",
     .method = {.args = root_peeler_args, .nargs = 2, .result = V_BOOL, .fn = method_root_peeler}                    },
    {.kind = M_METHOD,
     .name = "peeler_probe",
     .doc = "True if a file is a peeler-supported archive",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_peeler_probe}                 },
    {.kind = M_METHOD,
     .name = "hd_create",
     .doc = "Create a blank SCSI hard-disk image",
     .method = {.args = root_hd_create_args, .nargs = 2, .result = V_BOOL, .fn = method_root_hd_create}              },
    {.kind = M_METHOD,
     .name = "hd_download",
     .doc = "Export a hard-disk image (base + delta) to a flat file",
     .method = {.args = root_hd_download_args, .nargs = 2, .result = V_BOOL, .fn = method_root_hd_download}          },
    {.kind = M_METHOD,
     .name = "rom_probe",
     .doc = "True if a file is a recognised ROM image (no arg = is a ROM loaded?)",
     .method = {.args = root_path_arg_optional, .nargs = 1, .result = V_BOOL, .fn = method_root_rom_probe}           },
    {.kind = M_METHOD,
     .name = "rom_validate",
     .doc = "Verify a ROM file's checksum and recognised model",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_rom_validate}                 },
    {.kind = M_METHOD,
     .name = "vrom_probe",
     .doc = "True if a file is a recognised video-ROM image",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_vrom_probe}                   },
    {.kind = M_METHOD,
     .name = "vrom_validate",
     .doc = "Verify a video-ROM file's signature",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_vrom_validate}                },
    {.kind = M_METHOD,
     .name = "fd_probe",
     .doc = "True if a file is a recognised floppy-disk image",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_fd_probe}                     },
    {.kind = M_METHOD,
     .name = "find_media",
     .doc = "Search a directory for a floppy image; copy to dst if given",
     .method = {.args = root_find_media_args, .nargs = 2, .result = V_BOOL, .fn = method_root_find_media}            },
    {.kind = M_METHOD,
     .name = "partmap",
     .doc = "Parse and print the Apple Partition Map of a disk image",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_partmap}                      },
    {.kind = M_METHOD,
     .name = "probe",
     .doc = "Probe an image for its format (HFS / ISO / APM / ...)",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_probe}                        },
    {.kind = M_METHOD,
     .name = "list_partitions",
     .doc = "List partitions cached for a mounted image",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_list_partitions}              },
    {.kind = M_METHOD,
     .name = "unmount",
     .doc = "Force-close a cached auto-mount of an image",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_unmount}                      },
    {.kind = M_METHOD,
     .name = "quit",
     .doc = "Exit the emulator (asks the legacy quit command to end the run)",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = method_root_quit}                                  },
    {.kind = M_METHOD,
     .name = "assert",
     .doc = "Assert that a predicate is truthy; abort the script otherwise",
     .method = {.args = NULL, .nargs = 2, .result = V_BOOL, .fn = method_root_assert}                                },
    {.kind = M_METHOD,
     .name = "echo",
     .doc = "Print arguments separated by spaces (final newline appended)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_echo}                                  },
    // Checkpoint / runtime-state wrappers (M10b — checkpoint area).
    {.kind = M_METHOD,
     .name = "checkpoint_probe",
     .doc = "True if a valid checkpoint exists for the active machine",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_checkpoint_probe}                      },
    {.kind = M_METHOD,
     .name = "checkpoint_clear",
     .doc = "Remove all checkpoint files for the active machine",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_checkpoint_clear}                      },
    {.kind = M_METHOD,
     .name = "checkpoint_load",
     .doc = "Load a checkpoint (auto-loads latest when path is omitted)",
     .method = {.args = root_checkpoint_load_args, .nargs = 1, .result = V_BOOL, .fn = method_root_checkpoint_load}  },
    {.kind = M_METHOD,
     .name = "checkpoint_save",
     .doc = "Save the current machine state to a checkpoint file",
     .method = {.args = root_checkpoint_save_args, .nargs = 2, .result = V_BOOL, .fn = method_root_checkpoint_save}  },
    {.kind = M_METHOD,
     .name = "register_machine",
     .doc = "Register the active machine identity (must precede any image open)",
     .method = {.args = root_register_machine_args, .nargs = 2, .result = V_BOOL, .fn = method_root_register_machine}},
    {.kind = M_METHOD,
     .name = "running",
     .doc = "True if the scheduler is currently running",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_running}                               },
    {.kind = M_ATTR,
     .name = "auto_checkpoint",
     .doc = "Enable/disable the background auto-checkpoint loop (WASM-only)",
     .attr = {.type = V_BOOL, .get = attr_auto_checkpoint_get, .set = attr_auto_checkpoint_set}                      },
    // Drag-drop boot helpers (M10b — drop area).
    {.kind = M_METHOD,
     .name = "rom_checksum",
     .doc = "Return the 8-char hex checksum of a ROM file (empty on invalid)",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_STRING, .fn = method_root_rom_checksum}               },
    {.kind = M_METHOD,
     .name = "rom_load",
     .doc = "Load a ROM file and create the matching machine",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_rom_load}                     },
    {.kind = M_METHOD,
     .name = "fd_insert",
     .doc = "Insert a floppy image into a drive slot",
     .method = {.args = root_fd_insert_args, .nargs = 3, .result = V_BOOL, .fn = method_root_fd_insert}              },
    {.kind = M_METHOD,
     .name = "run",
     .doc = "Start the scheduler (optionally for a cycle budget)",
     .method = {.args = root_run_args, .nargs = 1, .result = V_BOOL, .fn = method_root_run}                          },
    // url-media + config-dialog + ui boot helpers (M10b — finish line).
    {.kind = M_METHOD,
     .name = "vrom_load",
     .doc = "Set the video-ROM path for the next machine init",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_vrom_load}                    },
    {.kind = M_METHOD,
     .name = "hd_attach",
     .doc = "Attach a hard-disk image at the given SCSI id",
     .method = {.args = root_hd_attach_args, .nargs = 2, .result = V_BOOL, .fn = method_root_hd_attach}              },
    {.kind = M_METHOD,
     .name = "hd_validate",
     .doc = "True if the file is a recognised hard-disk image",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_hd_validate}                  },
    {.kind = M_METHOD,
     .name = "cdrom_validate",
     .doc = "True if the file is a recognised CD-ROM image",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_cdrom_validate}               },
    {.kind = M_METHOD,
     .name = "cdrom_eject",
     .doc = "Eject the CD-ROM at the given SCSI id (default 3)",
     .method = {.args = root_cdrom_id_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_cdrom_eject}              },
    {.kind = M_METHOD,
     .name = "cdrom_info",
     .doc = "Print info for the CD-ROM at the given SCSI id (default 3)",
     .method = {.args = root_cdrom_id_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_cdrom_info}               },
    {.kind = M_METHOD,
     .name = "image_mounts",
     .doc = "List currently-mounted image paths (table format)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_image_mounts}                          },
    {.kind = M_METHOD,
     .name = "fd_validate",
     .doc = "Floppy density tag (\"400K\" / \"800K\" / \"1.4MB\") or empty if unrecognised",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_STRING, .fn = method_root_fd_validate}                },
    {.kind = M_METHOD,
     .name = "setup_machine",
     .doc = "Prime the next `rom_load` for a specific machine model",
     .method = {.args = root_setup_machine_args, .nargs = 2, .result = V_BOOL, .fn = method_root_setup_machine}      },
    {.kind = M_METHOD,
     .name = "schedule",
     .doc = "Set the scheduler mode (max / realtime / hardware)",
     .method = {.args = root_schedule_args, .nargs = 1, .result = V_BOOL, .fn = method_root_schedule}                },
    {.kind = M_METHOD,
     .name = "download",
     .doc = "Trigger a browser file download (WASM-only)",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_download}                     },
    {.kind = M_METHOD,
     .name = "cdrom_attach",
     .doc = "Attach a CD-ROM image to the SCSI bus",
     .method = {.args = root_path_arg, .nargs = 1, .result = V_BOOL, .fn = method_root_cdrom_attach}                 },
    {.kind = M_METHOD,
     .name = "hd_models",
     .doc = "Return the known SCSI HD model catalog as a JSON array string",
     .method = {.args = NULL, .nargs = 0, .result = V_STRING, .fn = method_root_hd_models}                           },
    // Debugging-area thin wrappers.
    {.kind = M_METHOD,
     .name = "disasm",
     .doc = "Disassemble forward from PC (legacy `d [count]`)",
     .method = {.args = NULL, .nargs = 1, .result = V_BOOL, .fn = method_root_disasm}                                },
    {.kind = M_METHOD,
     .name = "break_list_dump",
     .doc = "Print the breakpoint table (legacy `break list`)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_break_list_dump}                       },
    {.kind = M_METHOD,
     .name = "break_clear",
     .doc = "Clear all breakpoints (legacy `break clear`)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_break_clear}                           },
    {.kind = M_METHOD,
     .name = "logpoint_set",
     .doc = "Install a logpoint from a spec string (legacy `logpoint <spec>`)",
     .method = {.args = NULL, .nargs = 1, .result = V_BOOL, .fn = method_root_logpoint_set}                          },
    {.kind = M_METHOD,
     .name = "logpoint_list_dump",
     .doc = "Print the logpoint table (legacy `logpoint list`)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_logpoint_list_dump}                    },
    {.kind = M_METHOD,
     .name = "logpoint_clear",
     .doc = "Clear all logpoints (legacy `logpoint clear`)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_logpoint_clear}                        },
    {.kind = M_METHOD,
     .name = "log_set",
     .doc = "Set per-subsystem log level or full spec (legacy `log <subsys> <level|spec>`)",
     .method = {.args = NULL, .nargs = 2, .result = V_BOOL, .fn = method_root_log_set}                               },
    {.kind = M_METHOD,
     .name = "stop",
     .doc = "Interrupt the scheduler (legacy `stop`)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = method_root_stop}                                  },
    {.kind = M_METHOD,
     .name = "step",
     .doc = "Single-step N instructions; default 1 (legacy `step`/`s`)",
     .method = {.args = NULL, .nargs = 1, .result = V_BOOL, .fn = method_root_step}                                  },
    {.kind = M_METHOD,
     .name = "background_checkpoint",
     .doc = "Capture a checkpoint under the given label (legacy `background-checkpoint`)",
     .method = {.args = NULL, .nargs = 1, .result = V_BOOL, .fn = method_root_background_checkpoint}                 },
    {.kind = M_METHOD,
     .name = "path_exists",
     .doc = "True if the path exists in the shell VFS (legacy `exists`)",
     .method = {.args = NULL, .nargs = 1, .result = V_BOOL, .fn = method_root_path_exists}                           },
    {.kind = M_METHOD,
     .name = "exists",
     .doc = "True if the path exists in the shell VFS (alias for path_exists)",
     .method = {.args = NULL, .nargs = 1, .result = V_BOOL, .fn = method_root_path_exists}                           },
    {.kind = M_METHOD,
     .name = "path_size",
     .doc = "File size in bytes (legacy `size`)",
     .method = {.args = NULL, .nargs = 1, .result = V_UINT, .fn = method_root_path_size}                             },
    {.kind = M_METHOD,
     .name = "size",
     .doc = "File size in bytes (alias for path_size)",
     .method = {.args = NULL, .nargs = 1, .result = V_UINT, .fn = method_root_path_size}                             },
    {.kind = M_METHOD,
     .name = "fd_create",
     .doc = "Create a blank floppy image (legacy `fd create`)",
     .method = {.args = NULL, .nargs = 2, .result = V_BOOL, .fn = method_root_fd_create}                             },
    {.kind = M_METHOD,
     .name = "print_value",
     .doc = "Read a register / flag / memory cell (legacy `print <target>`)",
     .method = {.args = NULL, .nargs = 1, .result = V_UINT, .fn = method_root_print_value}                           },
    {.kind = M_METHOD,
     .name = "set_value",
     .doc = "Write a register / flag / memory cell (legacy `set <target> <value>`)",
     .method = {.args = NULL, .nargs = 2, .result = V_BOOL, .fn = method_root_set_value}                             },
    {.kind = M_METHOD,
     .name = "examine",
     .doc = "Hex-dump memory (legacy `x` / `examine`)",
     .method = {.args = NULL, .nargs = 2, .result = V_BOOL, .fn = method_root_examine}                               },
};

static const class_desc_t emu_root_class_real = {
    .name = "emu",
    .members = emu_root_members,
    .n_members = sizeof(emu_root_members) / sizeof(emu_root_members[0]),
};

// === Built-in alias registration ============================================
//
// Register at install time. Order matters only insofar as built-ins
// must be registered before any user can issue shell.alias.add — at
// startup that's guaranteed because user input arrives later.

static void register_builtin(const char *name, const char *path) {
    char err[160];
    if (alias_register_builtin(name, path, err, sizeof(err)) < 0)
        fprintf(stderr, "gs_classes: built-in alias '$%s' → '%s' rejected: %s\n", name, path, err);
}

static void register_cpu_aliases(void) {
    register_builtin("pc", "cpu.pc");
    register_builtin("sr", "cpu.sr");
    register_builtin("ccr", "cpu.ccr");
    register_builtin("ssp", "cpu.ssp");
    register_builtin("usp", "cpu.usp");
    register_builtin("msp", "cpu.msp");
    register_builtin("vbr", "cpu.vbr");
    register_builtin("sp", "cpu.sp");
    static const char *const dnames[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
    static const char *const anames[] = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
    for (int i = 0; i < 8; i++) {
        char path[16];
        snprintf(path, sizeof(path), "cpu.%s", dnames[i]);
        register_builtin(dnames[i], path);
        snprintf(path, sizeof(path), "cpu.%s", anames[i]);
        register_builtin(anames[i], path);
    }
}

static void register_fpu_aliases(void) {
    register_builtin("fpcr", "cpu.fpu.fpcr");
    register_builtin("fpsr", "cpu.fpu.fpsr");
    register_builtin("fpiar", "cpu.fpu.fpiar");
    static const char *const fpnames[] = {"fp0", "fp1", "fp2", "fp3", "fp4", "fp5", "fp6", "fp7"};
    for (int i = 0; i < 8; i++) {
        char path[24];
        snprintf(path, sizeof(path), "cpu.fpu.%s", fpnames[i]);
        register_builtin(fpnames[i], path);
    }
}

// === Install / uninstall ====================================================
//
// Lifecycle invariant: stubs are tied to a specific `cfg` pointer. Two
// patterns both have to work:
//
//   1. Cold boot
//        system_create(new) -> gs_classes_install(new)
//        ... later ...
//        system_destroy(new) -> gs_classes_uninstall_if(new)
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
// reinstalling. `gs_classes_uninstall_if(cfg)` (called from
// system_destroy) only tears down when the stubs are still associated
// with `cfg` — so destroying the old config after a load is a no-op.

#define MAX_STUBS 40
static struct object *g_stubs[MAX_STUBS];
static int g_stub_count = 0;
static struct config *g_installed_cfg = NULL;

static struct object *attach_stub(struct object *parent, const class_desc_t *cls, void *data, const char *name) {
    if (g_stub_count >= MAX_STUBS)
        return NULL;
    char err[200];
    if (!object_validate_class(cls, err, sizeof(err))) {
        fprintf(stderr, "gs_classes: class '%s' invalid: %s\n", cls->name ? cls->name : "?", err);
        return NULL;
    }
    struct object *o = object_new(cls, data, name);
    if (!o)
        return NULL;
    object_attach(parent ? parent : object_root(), o);
    g_stubs[g_stub_count++] = o;
    return o;
}

void gs_classes_install_root(void) {
    // Registers the top-level method table on the object root. Safe to
    // call repeatedly — object_root_set_class is idempotent for the
    // same class pointer.
    object_root_set_class(&emu_root_class_real);
}

void gs_classes_install(struct config *cfg) {
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
        gs_classes_uninstall();
    g_installed_cfg = cfg;

    // Top-level methods. Already installed by shell_init via
    // gs_classes_install_root; the call is repeated here so paths that
    // skip shell_init still get the methods.
    gs_classes_install_root();

    // Subsystem-scoped objects are registered by their owners (cpu_init,
    // memory_map_init, scc_init, rtc_init, via_init, scsi_init,
    // floppy_init, sound_init, appletalk_init, debug_init). What
    // remains here is process-/cfg-scoped state that has no per-machine
    // init function: stub namespace nodes, the storage view of
    // cfg->images, the lazy mac globals, the shell alias child, and
    // the platform-level mouse / keyboard / screen / vfs / find facades.
    /* scheduler */ attach_stub(NULL, &scheduler_class_desc, cfg, "scheduler");
    /* machine   */ attach_stub(NULL, &machine_class, cfg, "machine");
    struct object *shell_obj = attach_stub(NULL, &shell_class_desc, cfg, "shell");
    struct object *storage_obj = attach_stub(NULL, &storage_class_real, cfg, "storage");
    if (storage_obj) {
        attach_stub(storage_obj, &storage_images_collection_class, cfg, "images");
        storage_object_classes_init(cfg);
    }

    // mac is always attached — readers tolerate uninitialised RAM.

    // shell.alias child object.
    if (shell_obj)
        attach_stub(shell_obj, &shell_alias_class, cfg, "alias");

    attach_stub(NULL, &mouse_class, cfg, "mouse");
    attach_stub(NULL, &keyboard_class, cfg, "keyboard");
    attach_stub(NULL, &screen_class, cfg, "screen");
    attach_stub(NULL, &vfs_class, cfg, "vfs");
    attach_stub(NULL, &find_class, cfg, "find");

    // Built-in aliases. Register CPU always, FPU only when present,
    // mac always (the table is size-driven and machine-independent).
    register_cpu_aliases();
    if (cfg && cfg->cpu && cfg->cpu->fpu)
        register_fpu_aliases();
}

void gs_classes_uninstall(void) {
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
void gs_classes_uninstall_if(struct config *cfg) {
    if (g_installed_cfg == cfg)
        gs_classes_uninstall();
}
