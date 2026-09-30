// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_alias.c
// Command aliases (cmd_alias.h): the built-in set, and resolving a bare
// word to the method its alias names.

#include "cmd_alias.h"

#include "alias.h"

#include <stdio.h>
#include <string.h>

// The built-in command aliases: common shell verbs for the methods behind
// them.  Their targets resolve when the node exists (scheduler, machine and
// debug only while a machine runs).
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

void shell_command_aliases_install(void) {
    char err[160];
    for (size_t i = 0; i < sizeof(k_builtin) / sizeof(k_builtin[0]); i++)
        if (alias_register_builtin(k_builtin[i].name, k_builtin[i].target, err, sizeof(err)) < 0)
            fprintf(stderr, "command alias %s: %s\n", k_builtin[i].name, err);
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

bool shell_command_alias(const char *word, node_t *out, const char **target_out) {
    if (!word || !*word)
        return false;
    const char *path = alias_lookup(word, NULL);
    if (!path || !method_node(path, out))
        return false;
    if (target_out)
        *target_out = path;
    return true;
}

struct each_ctx {
    shell_command_alias_fn fn;
    void *ud;
};

static bool each_cb(const char *name, const char *path, alias_kind_t kind, void *ud) {
    struct each_ctx *c = (struct each_ctx *)ud;
    if (!method_node(path, NULL))
        return true;
    return c->fn(name, path, kind == ALIAS_BUILTIN, c->ud);
}

void shell_command_alias_each(shell_command_alias_fn fn, void *ud) {
    struct each_ctx c = {.fn = fn, .ud = ud};
    alias_each(each_cb, &c);
}
