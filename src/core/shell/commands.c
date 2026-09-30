// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// commands.c
// The command table (commands.h): built-in commands, `command NAME = PATH`
// declarations, and the `shell.command` node.

#include "commands.h"

#include "value.h"
#include "job/job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The built-in commands: common shell verbs for the methods behind them.
// A target resolves when its node exists (scheduler, machine and debug only
// while a machine runs).
static const struct {
    const char *name, *target;
} k_builtin[] = {
    {"ls",     "files.ls"      },
    {"cat",    "files.cat"     },
    {"cp",     "files.cp"      },
    {"mv",     "files.mv"      },
    {"rm",     "files.rm"      },
    {"mkdir",  "files.mkdir"   },
    {"cd",     "files.cd"      },
    {"pwd",    "files.pwd"     },
    {"run",    "scheduler.run" },
    {"stop",   "scheduler.stop"},
    {"reset",  "machine.reset" },
    {"step",   "debug.step"    },
    {"disasm", "debug.disasm"  },
};
#define N_BUILTIN (sizeof(k_builtin) / sizeof(k_builtin[0]))

// User commands, in declaration order.  Guarded by the job tables lock: the
// interpreter (job thread) and completion / highlighting (emulator thread)
// both read it.
typedef struct {
    char *name;
    char *target;
} user_cmd_t;

static user_cmd_t *g_user;
static size_t g_user_count, g_user_cap;

static int builtin_index(const char *name) {
    for (size_t i = 0; i < N_BUILTIN; i++)
        if (strcmp(k_builtin[i].name, name) == 0)
            return (int)i;
    return -1;
}

static int user_index(const char *name) {
    for (size_t i = 0; i < g_user_count; i++)
        if (strcmp(g_user[i].name, name) == 0)
            return (int)i;
    return -1;
}

// The method node `path` names, if it names one.
static bool method_node(const char *path, node_t *out) {
    node_t n = object_resolve(object_root(), path);
    if (!node_valid(n) || !n.member || n.member->kind != M_METHOD)
        return false;
    if (out)
        *out = n;
    return true;
}

int shell_command_define(const char *name, const char *target, char *err, size_t err_size) {
    if (!object_validate_name(name, err, err_size))
        return -1;
    if (builtin_index(name) >= 0) {
        snprintf(err, err_size, "'%s' is a built-in command", name);
        return -1;
    }
    if (node_valid(object_resolve(object_root(), name))) {
        snprintf(err, err_size, "'%s' is already a path at the root", name);
        return -1;
    }
    node_t n = object_resolve(object_root(), target);
    if (!node_valid(n)) {
        snprintf(err, err_size, "'%s' did not resolve", target);
        return -1;
    }
    if (!n.member || n.member->kind != M_METHOD) {
        snprintf(err, err_size, "'%s' is not a method (a command runs a method; use alias for a value)", target);
        return -1;
    }
    char *nm = strdup(name), *tg = strdup(target);
    if (!nm || !tg) {
        free(nm);
        free(tg);
        snprintf(err, err_size, "out of memory");
        return -1;
    }
    job_tables_lock();
    int i = user_index(name);
    if (i >= 0) {
        free(g_user[i].name);
        free(g_user[i].target);
        g_user[i] = (user_cmd_t){nm, tg};
        job_tables_unlock();
        return 0;
    }
    if (g_user_count == g_user_cap) {
        size_t cap = g_user_cap ? g_user_cap * 2 : 8;
        user_cmd_t *grown = (user_cmd_t *)realloc(g_user, cap * sizeof(*grown));
        if (!grown) {
            job_tables_unlock();
            free(nm);
            free(tg);
            snprintf(err, err_size, "out of memory");
            return -1;
        }
        g_user = grown;
        g_user_cap = cap;
    }
    g_user[g_user_count++] = (user_cmd_t){nm, tg};
    job_tables_unlock();
    return 0;
}

int shell_command_remove(const char *name, char *err, size_t err_size) {
    if (builtin_index(name) >= 0) {
        snprintf(err, err_size, "'%s' is a built-in command", name);
        return -1;
    }
    job_tables_lock();
    int i = user_index(name);
    if (i < 0) {
        job_tables_unlock();
        snprintf(err, err_size, "no command '%s'", name);
        return -1;
    }
    free(g_user[i].name);
    free(g_user[i].target);
    memmove(&g_user[i], &g_user[i + 1], (g_user_count - (size_t)i - 1) * sizeof(*g_user));
    g_user_count--;
    job_tables_unlock();
    return 0;
}

bool shell_command_lookup(const char *word, node_t *out, char *target, size_t target_size) {
    if (!word || !*word)
        return false;
    char path[256];
    int b = builtin_index(word);
    if (b >= 0) {
        snprintf(path, sizeof(path), "%s", k_builtin[b].target);
    } else {
        job_tables_lock();
        int i = user_index(word);
        if (i >= 0)
            snprintf(path, sizeof(path), "%s", g_user[i].target);
        job_tables_unlock();
        if (i < 0)
            return false;
    }
    if (!method_node(path, out))
        return false;
    if (target && target_size)
        snprintf(target, target_size, "%s", path);
    return true;
}

void shell_command_each(shell_command_fn fn, void *ud) {
    for (size_t i = 0; i < N_BUILTIN; i++)
        if (!fn(k_builtin[i].name, k_builtin[i].target, true, ud))
            return;
    // Copy under the lock: the callback may resolve paths or allocate.
    job_tables_lock();
    size_t n = g_user_count;
    user_cmd_t *copy = n ? (user_cmd_t *)calloc(n, sizeof(*copy)) : NULL;
    for (size_t i = 0; copy && i < n; i++)
        copy[i] = (user_cmd_t){strdup(g_user[i].name), strdup(g_user[i].target)};
    job_tables_unlock();
    bool go = true;
    for (size_t i = 0; copy && i < n; i++) {
        if (go && copy[i].name && copy[i].target)
            go = fn(copy[i].name, copy[i].target, false, ud);
        free(copy[i].name);
        free(copy[i].target);
    }
    free(copy);
}

void shell_command_clear_user(void) {
    job_tables_lock();
    for (size_t i = 0; i < g_user_count; i++) {
        free(g_user[i].name);
        free(g_user[i].target);
    }
    g_user_count = 0;
    job_tables_unlock();
}

// === shell.command ============================================================

static value_t method_command_add(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    char err[200];
    if (shell_command_define(argv[0].s, argv[1].s, err, sizeof(err)) < 0)
        return val_err("command: %s", err);
    return val_none();
}

static value_t method_command_remove(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    char err[200];
    if (shell_command_remove(argv[0].s, err, sizeof(err)) < 0)
        return val_err("command: %s", err);
    return val_none();
}

typedef struct {
    value_t *items;
    size_t len, cap;
} list_acc_t;

static bool list_collect(const char *name, const char *target, bool builtin, void *ud) {
    list_acc_t *acc = (list_acc_t *)ud;
    node_t n;
    bool ok = method_node(target, &n);
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "name", val_str(name));
    val_map_put(b, "target", val_str(target));
    val_map_put(b, "doc", val_str(ok && n.member->doc ? n.member->doc : ""));
    val_map_put(b, "builtin", val_bool(builtin));
    val_map_put(b, "available", val_bool(ok));
    return val_list_push(&acc->items, &acc->len, &acc->cap, val_map_finish(b));
}

static value_t method_command_list(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    list_acc_t acc = {0};
    shell_command_each(list_collect, &acc);
    return val_list(acc.items, acc.len);
}

static const arg_decl_t command_add_args[] = {
    {.name = "name", .kind = V_STRING, .doc = "The word typed bare"       },
    {.name = "path", .kind = V_STRING, .doc = "Path of the method it runs"},
};
static const arg_decl_t command_remove_args[] = {
    {.name = "name", .kind = V_STRING, .doc = "A user command's word"},
};

static const member_t shell_command_members[] = {
    {.kind = M_METHOD,
     .name = "add",
     .examples = (const char *const[]){"shell.command.add ll files.list", NULL},
     .doc = "Declare a user command (the same as `command NAME = PATH`)",
     .method = {.args = command_add_args, .nargs = 2, .result = V_NONE, .fn = method_command_add}      },
    {.kind = M_METHOD,
     .name = "remove",
     .examples = (const char *const[]){"shell.command.remove ll", NULL},
     .doc = "Remove a user command",
     .method = {.args = command_remove_args, .nargs = 1, .result = V_NONE, .fn = method_command_remove}},
    {.kind = M_METHOD,
     .name = "list",
     .examples = (const char *const[]){"shell.command.list", NULL},
     .doc = "Every command, built-ins first",
     .method = {.result_doc = "[{name, target, doc, builtin, available}]; available: the target is a method "
                              "right now",
                .args = NULL,
                .nargs = 0,
                .result = V_LIST,
                .fn = method_command_list}                                                             },
};

const class_desc_t shell_command_class = {
    .name = "command",
    .doc = "Commands: bare words that run a method (ls, cd, run, …)",
    .members = shell_command_members,
    .n_members = sizeof(shell_command_members) / sizeof(shell_command_members[0]),
};
