// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_alias.h
// Command aliases: an alias whose target is a method can be typed as a bare
// word at the head of a statement (`ls /opfs` runs `files.ls /opfs`).  The
// alias table is the one `$name` uses (alias.h); only method targets take
// part, so an alias of an attribute or a node -- `$pc`, the Mac globals --
// stays `$`-only.  A tree path, a keyword or a `def` function of the same
// name always wins.

#ifndef GS_SHELL_CMD_ALIAS_H
#define GS_SHELL_CMD_ALIAS_H

#include <stdbool.h>

#include "object.h"

// Resolve `word` (a bare identifier) as a command alias.  On success fills
// `out` with the target method's node and, when `target_out` is non-NULL,
// the target path (the alias table's own storage).
bool shell_command_alias(const char *word, node_t *out, const char **target_out);

// Visit every command alias whose target resolves to a method right now,
// in registration order.  The callback returns false to stop.
typedef bool (*shell_command_alias_fn)(const char *name, const char *target, bool builtin, void *ud);
void shell_command_alias_each(shell_command_alias_fn fn, void *ud);

// Register the built-in command aliases (idempotent).
void shell_command_aliases_install(void);

#endif // GS_SHELL_CMD_ALIAS_H
