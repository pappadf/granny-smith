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
#include "value.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// `log.set(category, level=, stdout=, file=, ts=, pc=)` — per-subsystem
// logging, with real named arguments.
//
// The second slot used to be declared V_NONE -- no type at all -- and accept
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
static value_t log_method_set(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    if (argv[0].kind != V_ENUM || !argv[0].enm.table)
        return val_err("log.set: category is required");
    const char *category = argv[0].enm.table[argv[0].enm.idx];

    bool touched = false;

    if (argv[1].kind != V_NONE) {
        bool ok = false;
        int64_t level = val_as_i64(&argv[1], &ok);
        if (!ok || level < 0)
            return val_err("log.set: level must be a non-negative integer");
        if (log_set_category_level(category, (int)level) != 0)
            return val_err("log.set: cannot set level on '%s'", category);
        touched = true;
    }
    if (argv[2].kind == V_BOOL) {
        log_set_category_stdout(category, argv[2].b);
        touched = true;
    }
    if (argv[3].kind == V_STRING && argv[3].s) {
        if (log_set_category_file(category, argv[3].s) != 0)
            return val_err("log.set: cannot open log file '%s'", argv[3].s);
        touched = true;
    }
    if (argv[4].kind == V_BOOL) {
        log_set_category_timestamp(category, argv[4].b);
        touched = true;
    }
    if (argv[5].kind == V_BOOL) {
        log_set_category_show_pc(category, argv[5].b);
        touched = true;
    }

    log_print_category(category);
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
static value_t log_attr_levels(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
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
// `file`, which sits between them.  A V_NONE default is filled in and skips
// validation, which is precisely "the caller did not mention this one" and is
// what the body below tests for.

static const arg_decl_t log_set_args[] = {
    {.name = "category", .kind = V_ENUM, .enum_values = log_category_values, .doc = "Subsystem to configure"},
    {.name = "level",
     .default_value = &obj_arg_unset,
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Verbosity; 0 silences level-1-and-up sites"},
    {.name = "stdout",
     .default_value = &obj_arg_unset,
     .kind = V_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Emit to stdout"},
    {.name = "file",
     .default_value = &obj_arg_unset,
     .kind = V_STRING,
     .presentation_flags = VAL_PATH,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Append to this path; \"off\" closes it"},
    {.name = "ts",
     .default_value = &obj_arg_unset,
     .kind = V_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Stamp each line with a timestamp"},
    {.name = "pc",
     .default_value = &obj_arg_unset,
     .kind = V_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Stamp each line with the guest PC"},
};

// === log.category["<name>"] =================================================
//
// One entry object per category, made the first time the category is looked
// up and kept for the process (categories are never unregistered).  Its data
// is the log_category_t.

#define LOG_MAX_CATEGORIES 128

static struct object *g_log_object = NULL;
static struct object *g_log_categories_object = NULL;
static struct object *g_cat_objs[LOG_MAX_CATEGORIES];
static const log_category_t *g_cat_ptrs[LOG_MAX_CATEGORIES];
static int g_cat_n = 0;

static const log_category_t *entry_cat(struct object *self) {
    return (const log_category_t *)object_data(self);
}

static value_t cat_attr_level_get(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, (uint64_t)log_get_level(entry_cat(self)));
}

static value_t cat_attr_level_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    bool ok = false;
    int64_t level = val_as_i64(&in, &ok);
    value_free(&in);
    if (!ok || level < 0 || level > INT32_MAX)
        return val_err("level must be a non-negative integer");
    log_set_category_level(log_category_name(entry_cat(self)), (int)level);
    return val_none();
}

static value_t cat_attr_stdout_get(struct object *self, const member_t *m) {
    (void)m;
    return val_bool(log_get_category_stdout(entry_cat(self)));
}

static value_t cat_attr_stdout_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    log_set_category_stdout(log_category_name(entry_cat(self)), in.b);
    return val_none();
}

static value_t cat_attr_file_get(struct object *self, const member_t *m) {
    (void)m;
    const char *path = log_get_category_file(entry_cat(self));
    return val_str(path ? path : "off");
}

static value_t cat_attr_file_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    const char *path = in.s ? in.s : "off";
    int rc = log_set_category_file(log_category_name(entry_cat(self)), path);
    value_t out = rc == 0 ? val_none() : val_err("cannot open log file '%s'", path);
    value_free(&in);
    return out;
}

static value_t cat_attr_ts_get(struct object *self, const member_t *m) {
    (void)m;
    return val_bool(log_get_category_timestamp(entry_cat(self)));
}

static value_t cat_attr_ts_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    log_set_category_timestamp(log_category_name(entry_cat(self)), in.b);
    return val_none();
}

static value_t cat_attr_pc_get(struct object *self, const member_t *m) {
    (void)m;
    return val_bool(log_get_category_show_pc(entry_cat(self)));
}

static value_t cat_attr_pc_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    log_set_category_show_pc(log_category_name(entry_cat(self)), in.b);
    return val_none();
}

static const member_t log_category_members[] = {
    {.kind = M_ATTR,
     .name = "level",
     .doc = "Verbosity: 0 is silent, higher is more verbose",
     .attr = {.type = V_UINT, .width = 4, .get = cat_attr_level_get, .set = cat_attr_level_set}},
    {.kind = M_ATTR,
     .name = "stdout",
     .doc = "Emit this category's lines to stdout (the console)",
     .attr = {.type = V_BOOL, .get = cat_attr_stdout_get, .set = cat_attr_stdout_set}          },
    {.kind = M_ATTR,
     .name = "file",
     .doc = "Also append to this path; \"off\" when closed (assigning \"off\" closes it)",
     .attr = {.type = V_STRING, .get = cat_attr_file_get, .set = cat_attr_file_set}            },
    {.kind = M_ATTR,
     .name = "ts",
     .doc = "Stamp each line with a timestamp",
     .attr = {.type = V_BOOL, .get = cat_attr_ts_get, .set = cat_attr_ts_set}                  },
    {.kind = M_ATTR,
     .name = "pc",
     .doc = "Stamp each line with the guest PC",
     .attr = {.type = V_BOOL, .get = cat_attr_pc_get, .set = cat_attr_pc_set}                  },
};

static const class_desc_t log_category_class = {
    .name = "log_category",
    .members = log_category_members,
    .n_members = sizeof(log_category_members) / sizeof(log_category_members[0]),
};

// The entry object for a registered category, made on first use.
static struct object *category_entry(const char *name) {
    const log_category_t *cat = log_get_category(name);
    if (!cat)
        return NULL;
    for (int i = 0; i < g_cat_n; i++)
        if (g_cat_ptrs[i] == cat)
            return g_cat_objs[i];
    if (g_cat_n >= LOG_MAX_CATEGORIES)
        return NULL;
    struct object *o = object_new(&log_category_class, (void *)cat, NULL);
    if (!o)
        return NULL;
    object_set_logical_parent(o, g_log_categories_object, NULL, -1, log_category_name(cat));
    object_set_doc(o, log_category_description(log_category_name(cat)));
    g_cat_ptrs[g_cat_n] = cat;
    g_cat_objs[g_cat_n] = o;
    g_cat_n++;
    return o;
}

static struct object *categories_lookup(struct object *self, const char *name) {
    (void)self;
    return category_entry(name);
}

// Key scratch for categories_keys: borrowed by the caller until the next call.
static const char *g_key_scratch[LOG_MAX_CATEGORIES];
static int g_key_n;

static void collect_key(const log_category_t *cat, void *ud) {
    (void)ud;
    if (g_key_n < LOG_MAX_CATEGORIES)
        g_key_scratch[g_key_n++] = log_category_name(cat);
}

// Keys in name order, so listings are stable whatever order categories
// registered in.
static int key_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int categories_keys(struct object *self, const char ***out_names) {
    (void)self;
    g_key_n = 0;
    log_foreach_category(collect_key, NULL);
    qsort(g_key_scratch, (size_t)g_key_n, sizeof(g_key_scratch[0]), key_cmp);
    *out_names = g_key_scratch;
    return g_key_n;
}

static const member_t log_categories_members[] = {
    {.kind = M_CHILD,
     .name = "entries",
     .doc = "Log categories, by name",
     .child = {.cls = &log_category_class, .indexed = true, .lookup = categories_lookup, .keys = categories_keys}},
};

static const class_desc_t log_categories_class = {
    .name = "log_categories",
    .members = log_categories_members,
    .n_members = sizeof(log_categories_members) / sizeof(log_categories_members[0]),
    .doc = "Log categories, by name",
};

static const member_t log_members[] = {
    {.kind = M_ATTR,
     .name = "levels",
     .doc = "Every registered log category and its level, as a map {<category>: <level>}",
     .flags = VAL_RO,
     .attr = {.type = V_MAP, .get = log_attr_levels}},
    {.kind = M_METHOD,
     .name = "set",
     .doc = "Configure a log category: log.set(cat, level=, stdout=, file=, ts=, pc=)",
     .method = {.ui_flags = MM_MUTATE, .args = log_set_args, .nargs = 6, .result = V_BOOL, .fn = log_method_set}},
};

static const class_desc_t log_class = {
    .name = "log",
    .members = log_members,
    .n_members = sizeof(log_members) / sizeof(log_members[0]),
    .doc = "Logging configuration: per-category levels and sinks",
    .task = "log",
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
    g_log_categories_object = object_new(&log_categories_class, NULL, "category");
    if (g_log_categories_object) {
        object_set_label(g_log_categories_object, "Categories");
        object_attach(g_log_object, g_log_categories_object);
    }
}
