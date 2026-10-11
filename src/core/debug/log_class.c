// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// log_class.c
// The root `log` node: logging configuration as objects.
//
//   log.set(category, level=, stdout=, file=, ts=, pc=)   configure one category
//   log.levels                                              {category: level}
//   log.category["scsi"].level = 5                          one category's settings
//
// `log.category` is a keyed collection over every registered category
// (log_register_manifest registers the whole manifest at setup).  Levels are
// numeric: 0 is silent, higher is more verbose.  A process singleton, created
// at shell init.

#include "log.h"
#include "log_categories.h"
#include "object.h"
#include "out.h"
#include "value.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Prints one category's settings, as `log.set <cat>` reports them
static void print_category(const char *category) {
    const log_category_t *c = log_get_category(category);
    if (!c) {
        out_printf("unknown category \"%s\" (see log.levels for the full list)\n", category);
        return;
    }
    const char *file = log_get_category_file(c);
    out_printf("%s level=%d stdout=%s file=%s ts=%s pc=%s\n", log_category_name(c), log_get_level(c),
               log_get_category_stdout(c) ? "on" : "off", file ? file : "off",
               log_get_category_timestamp(c) ? "on" : "off", log_get_category_show_pc(c) ? "on" : "off");
}

// `log.set(category, level=, stdout=, file=, ts=, pc=)` — per-subsystem
// logging, with real named arguments.
//
// The second slot used to be declared VK_NONE -- no type at all -- and accept
// either an integer or a spec string like "level=5 file=tmp/foo.txt
// stdout=off ts=on", which the body then parsed itself with strtok_r.  So the
// framework validated nothing (it had been told nothing to validate),
// completion could offer neither the keys nor their values, and the method
// carried its own boolean vocabulary and its own error wording.
// docs/internals/core/object/object-model.md ("Library conventions") says in as many
// words that named arguments exist to retire exactly this: "no flag
// grammars inside strings".
//
// `log.set(cat)` with nothing else prints the category's current settings,
// which is what the bare form always did.
static DEF_METHOD(log_method_set) {
    if (argv[0].kind != VK_ENUM || !argv[0].enm.table)
        return val_err("log.set: category is required");
    const char *category = argv[0].enm.table[argv[0].enm.idx];

    bool touched = false;

    if (argv[1].kind != VK_NONE) {
        bool ok = false;
        int64_t level = val_as_i64(&argv[1], &ok);
        if (!ok || level < 0)
            return val_err("log.set: level must be a non-negative integer");
        if (log_set_category_level(category, (int)level) != 0)
            return val_err("log.set: cannot set level on '%s'", category);
        touched = true;
    }
    if (argv[2].kind == VK_BOOL) {
        log_set_category_stdout(category, argv[2].b);
        touched = true;
    }
    if (argv[3].kind == VK_STRING && argv[3].s) {
        if (log_set_category_file(category, argv[3].s) != 0) {
            int err = errno; // set on every failure, file or not (log.h)
            return val_err("log.set: cannot set log file '%s': %s", argv[3].s, strerror(err));
        }
        touched = true;
    }
    if (argv[4].kind == VK_BOOL) {
        log_set_category_timestamp(category, argv[4].b);
        touched = true;
    }
    if (argv[5].kind == VK_BOOL) {
        log_set_category_show_pc(category, argv[5].b);
        touched = true;
    }

    print_category(category);
    (void)touched;
    return val_bool(true);
}

// log_foreach_category callback: put "<category>" → <level> into the map.
static void log_level_map_cb(const log_category_t *cat, void *ud) {
    value_map_builder_t *b = (value_map_builder_t *)ud;
    val_map_put(b, log_category_name(cat), val_uint(4, (uint64_t)log_get_level(cat)));
}

// `log.levels` — every registered category and its current level, as a
// map {<category>: <level>, ...}.  The Logs view's level editor reads
// this to populate its list (the categories register lazily, so the set grows
// as subsystems first log; a freshly booted machine has registered its own).
static DEF_GETTER(log_attr_levels) {
    value_map_builder_t *b = val_map_new();
    log_foreach_category(log_level_map_cb, b);
    return val_map_finish(b);
}

// Every category the manifest declares, as an enum table, so the framework
// rejects a typo and completion can offer all 62 names.
static const char *const log_category_values[] = {
#define X(n, lvl, desc) n,
    GS_LOG_CATEGORIES(X)
#undef X
        NULL};

// "Not supplied", as a default.
//
// An optional slot with no default_value cannot be a HOLE before a later
// given slot -- node_validate_args says so directly: "argc truncation only
// works at the tail".  So `log.set(cpu, level=3, ts=on)` would fail on
// `file`, which sits between them.  A VK_NONE default is filled in and skips
// validation, which is precisely "the caller did not mention this one" and is
// what the body below tests for.

static const arg_decl_t log_set_args[] = {
    {.name = "category", .kind = VK_ENUM, .enum_values = log_category_values, .doc = "Subsystem to configure"},
    {.name = "level",
     .kind = VK_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Verbosity; 0 silences level-1-and-up sites"},
    {.name = "stdout", .kind = VK_BOOL, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "Emit to stdout"},
    {.name = "file",
     .kind = VK_STRING,
     .presentation_flags = VFLAG_PATH,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Append to this path; \"off\" closes it"},
    {.name = "ts", .kind = VK_BOOL, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "Stamp each line with a timestamp"},
    {.name = "pc", .kind = VK_BOOL, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "Stamp each line with the guest PC"},
};

// === log.category["<name>"] =================================================
//
// One entry object per category, made the first time the category is looked
// up and kept for the process (categories are never unregistered).  Its data
// is the log_category_t.

static struct object *g_log_object = NULL;

static const log_category_t *entry_cat(struct object *self) {
    return (const log_category_t *)object_data(self);
}

static DEF_GETTER(cat_attr_level_get) {
    return val_uint(4, (uint64_t)log_get_level(entry_cat(self)));
}

static DEF_SETTER(cat_attr_level_set) {
    bool ok = false;
    int64_t level = val_as_i64(&in, &ok);
    value_free(&in);
    if (!ok || level < 0 || level > INT32_MAX)
        return val_err("level must be a non-negative integer");
    log_set_category_level(log_category_name(entry_cat(self)), (int)level);
    return val_none();
}

static DEF_GETTER(cat_attr_stdout_get) {
    return val_bool(log_get_category_stdout(entry_cat(self)));
}

static DEF_SETTER(cat_attr_stdout_set) {
    log_set_category_stdout(log_category_name(entry_cat(self)), in.b);
    return val_none();
}

static DEF_GETTER(cat_attr_file_get) {
    const char *path = log_get_category_file(entry_cat(self));
    return val_str(path ? path : "off");
}

static DEF_SETTER(cat_attr_file_set) {
    const char *path = in.s ? in.s : "off";
    int rc = log_set_category_file(log_category_name(entry_cat(self)), path);
    int err = errno; // set on every failure, file or not (log.h)
    value_t out = rc == 0 ? val_none() : val_err("cannot set log file '%s': %s", path, strerror(err));
    value_free(&in);
    return out;
}

static DEF_GETTER(cat_attr_ts_get) {
    return val_bool(log_get_category_timestamp(entry_cat(self)));
}

static DEF_SETTER(cat_attr_ts_set) {
    log_set_category_timestamp(log_category_name(entry_cat(self)), in.b);
    return val_none();
}

static DEF_GETTER(cat_attr_pc_get) {
    return val_bool(log_get_category_show_pc(entry_cat(self)));
}

static DEF_SETTER(cat_attr_pc_set) {
    log_set_category_show_pc(log_category_name(entry_cat(self)), in.b);
    return val_none();
}

static const member_t log_category_members[] = {
    {.kind = MK_ATTR,
     .name = "level",
     .doc = "Verbosity: 0 is silent, higher is more verbose",
     .attr = {.type = VK_UINT, .width = 4, .get = cat_attr_level_get, .set = cat_attr_level_set}},
    {.kind = MK_ATTR,
     .name = "stdout",
     .doc = "Emit this category's lines to stdout (the console)",
     .attr = {.type = VK_BOOL, .get = cat_attr_stdout_get, .set = cat_attr_stdout_set}          },
    {.kind = MK_ATTR,
     .name = "file",
     .doc = "Also append to this path; \"off\" when closed (assigning \"off\" closes it)",
     .attr = {.type = VK_STRING, .get = cat_attr_file_get, .set = cat_attr_file_set}            },
    {.kind = MK_ATTR,
     .name = "ts",
     .doc = "Stamp each line with a timestamp",
     .attr = {.type = VK_BOOL, .get = cat_attr_ts_get, .set = cat_attr_ts_set}                  },
    {.kind = MK_ATTR,
     .name = "pc",
     .doc = "Stamp each line with the guest PC",
     .attr = {.type = VK_BOOL, .get = cat_attr_pc_get, .set = cat_attr_pc_set}                  },
};

static const class_desc_t log_category_class = {
    .name = "log_category",
    .members = log_category_members,
    .n_members = sizeof(log_category_members) / sizeof(log_category_members[0]),
};

// The category entry objects, by name, made on first use.
static object_cache_t g_categories = OBJECT_CACHE(&log_category_class, NULL);

// The entry object for a registered category, made on first use.
static struct object *categories_lookup(struct object *self, const char *name) {
    (void)self;
    const log_category_t *cat = log_get_category(name);
    if (!cat)
        return NULL;
    struct object *o = object_cache_key(&g_categories, log_category_name(cat), (void *)cat);
    if (o)
        object_set_doc(o, log_category_description(log_category_name(cat)));
    return o;
}

// The least category name after `prev` (NULL = before all), so listings are
// in name order whatever order categories registered in.
typedef struct {
    const char *prev;
    const char *best;
} next_name_t;

static void next_name_cb(const log_category_t *cat, void *ud) {
    next_name_t *n = (next_name_t *)ud;
    const char *name = log_category_name(cat);
    if (n->prev && strcmp(name, n->prev) <= 0)
        return;
    if (!n->best || strcmp(name, n->best) < 0)
        n->best = name;
}

static const char *categories_next_key(struct object *self, const char *prev) {
    (void)self;
    next_name_t n = {.prev = prev, .best = NULL};
    log_foreach_category(next_name_cb, &n);
    return n.best;
}

static const collection_desc_t log_categories = {
    .entry = &log_category_class,
    .by_key = {.lookup = categories_lookup, .next_key = categories_next_key},
    .name = "log_categories",
    .doc = "Log categories, by name",
    .entries_doc = "Log categories, by name",
};

static const member_t log_members[] = {
    {.kind = MK_ATTR,
     .name = "levels",
     .doc = "Every registered log category and its level, as a map {<category>: <level>}",
     .attr = {.type = VK_MAP, .get = log_attr_levels}                                                            },
    {.kind = MK_METHOD,
     .name = "set",
     .doc = "Configure a log category: log.set(cat, level=, stdout=, file=, ts=, pc=)",
     .method = {.ui_flags = MM_MUTATE, .args = log_set_args, .nargs = 6, .result = VK_BOOL, .fn = log_method_set}},
};

static const class_desc_t log_class = {
    .name = "log",
    .members = log_members,
    .n_members = sizeof(log_members) / sizeof(log_members[0]),
    .doc = "Logging configuration: per-category levels and sinks",
};

void log_class_init(void) {
    if (g_log_object)
        return;
    g_log_object = object_new(&log_class, NULL, "log");
    if (!g_log_object)
        return;
    object_set_label(g_log_object, "Logs");
    object_set_order(g_log_object, 50);
    object_attach(object_root(), g_log_object);
    struct object *categories = object_collection_new(&log_categories, NULL, "category");
    if (categories) {
        object_set_label(categories, "Categories");
        object_attach(g_log_object, categories);
        object_cache_set_parent(&g_categories, categories);
    }
}
