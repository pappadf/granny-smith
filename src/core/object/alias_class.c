// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// alias_class.c
// The `shell.alias` class: alias add / remove / list as object methods,
// over the alias table in alias.c. Kept apart from the table so the
// substrate carries no object-model class of its own.

#include "alias.h"

#include <stdio.h>

#include "object.h"
#include "status.h"
#include "value.h"

static DEF_METHOD(alias_method_add) {
    char err[160];
    if (alias_add_user(argv[0].s, argv[1].s, err, sizeof(err)) != STATUS_OK)
        return val_err("%s", err);
    return val_none();
}

static DEF_METHOD(alias_method_remove) {
    char err[160];
    if (alias_remove_user(argv[0].s, err, sizeof(err)) != STATUS_OK)
        return val_err("%s", err);
    return val_none();
}

// shell.alias.list builds a VK_LIST of VK_STRING entries: each "name=path".
typedef struct {
    value_t *items;
    size_t len;
    size_t cap;
} list_acc_t;

static bool list_acc_collect(const char *name, const char *path, alias_kind_t kind, void *ud) {
    list_acc_t *acc = (list_acc_t *)ud;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s=%s%s", name, path, kind == AK_BUILTIN ? " (built-in)" : "");
    // The shared accumulator; this was the third of five copies.
    if (!val_list_push(&acc->items, &acc->len, &acc->cap, val_str(buf)))
        return false;
    return true;
}

static DEF_METHOD(alias_method_list) {
    list_acc_t acc = {0};
    alias_each(list_acc_collect, &acc);
    return val_list(acc.items, acc.len);
}

static const arg_decl_t alias_add_args[] = {
    {.name = "name", .kind = VK_STRING, .doc = "alias identifier (no $)"          },
    {.name = "path", .kind = VK_STRING, .doc = "object path the alias substitutes"},
};
static const arg_decl_t alias_remove_args[] = {
    {.name = "name", .kind = VK_STRING, .doc = "alias identifier (no $)"},
};

static const member_t shell_alias_members[] = {
    {.kind = MK_METHOD,
     .name = "add",
     .doc = "Register a user alias",
     .flags = 0,
     .method = {.args = alias_add_args, .nargs = 2, .result = VK_NONE, .fn = alias_method_add}      },
    {.kind = MK_METHOD,
     .name = "remove",
     .doc = "Remove a user alias",
     .flags = 0,
     .method = {.args = alias_remove_args, .nargs = 1, .result = VK_NONE, .fn = alias_method_remove}},
    {.kind = MK_METHOD,
     .name = "list",
     .doc = "List aliases as 'name=path' strings",
     .flags = 0,
     .method = {.args = NULL, .nargs = 0, .result = VK_LIST, .fn = alias_method_list}               },
};

const class_desc_t shell_alias_class = {
    .name = "alias",
    .doc = "User and built-in aliases: add, remove, list",
    .members = shell_alias_members,
    .n_members = sizeof(shell_alias_members) / sizeof(shell_alias_members[0]),
};
