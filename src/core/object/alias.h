// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// alias.h
// Two-tier alias table for `$name` substitution. See
// proposal-module-object-model.md §4.4.
//
// - **Built-in aliases** are registered by classes / the framework at
//   init time (alias_register_builtin). Re-registration of the same
//   built-in is a no-op (idempotent across emulator restarts). User
//   add/remove cannot touch them.
// - **User aliases** are added/removed at runtime (alias_add_user,
//   alias_remove_user). They cannot collide with built-ins or with
//   reserved words.
//
// Aliases are session-only — no persistence layer in M3. Reset on
// emulator destroy via alias_reset().

#ifndef GS_OBJECT_ALIAS_H
#define GS_OBJECT_ALIAS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ALIAS_BUILTIN = 1,
    ALIAS_USER,
} alias_kind_t;

// Register a built-in alias. Idempotent: registering the same
// (name, path) twice is a no-op. Registering a built-in with a name
// that already exists as a different built-in fails.
//
// Returns 0 on success, negative on failure (reserved word, name
// already a user alias and would conflict, etc.). On failure
// `err_buf` (if non-NULL) receives a one-line message.
int alias_register_builtin(const char *name, const char *path, char *err_buf, size_t err_size);

// User-side `shell.alias.add NAME PATH`. Fails if name is a reserved
// word or collides with a built-in. Replaces an existing user alias
// of the same name.
int alias_add_user(const char *name, const char *path, char *err_buf, size_t err_size);

// User-side `shell.alias.remove NAME`. Fails if name is a built-in or
// no user alias of that name exists.
int alias_remove_user(const char *name, char *err_buf, size_t err_size);

// Look up the path for `name`. Returns a pointer into the table's
// own storage (valid until the alias is removed) or NULL if no such
// alias exists. Optionally writes the alias kind into `kind_out`.
const char *alias_lookup(const char *name, alias_kind_t *kind_out);

// Iterate every alias in registration order. The callback returns
// `true` to continue, `false` to stop early.
typedef bool (*alias_iter_fn)(const char *name, const char *path, alias_kind_t kind, void *ud);
void alias_each(alias_iter_fn fn, void *ud);

// Number of currently registered aliases (built-in + user).
size_t alias_count(void);

// Drop every alias (both tiers). Tests use this to start clean.
void alias_reset(void);

// Drop only user aliases — used by checkpoint restore (per
// proposal §4.4.5: checkpoints don't preserve user aliases).
void alias_clear_user(void);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_ALIAS_H
