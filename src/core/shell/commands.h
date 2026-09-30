// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// commands.h
// Commands: bare words that run a method (`ls /opfs` runs `files.ls /opfs`),
// declared with `command NAME = PATH` or registered as built-ins.  A table
// of their own, apart from `$name` aliases (alias.h): an alias is a value or
// a place, used anywhere with `$`; a command is a verb, used as the first
// word of a statement.  A tree path, a keyword or a `def` function of the
// same name always wins over a command.

#ifndef GS_SHELL_COMMANDS_H
#define GS_SHELL_COMMANDS_H

#include <stdbool.h>
#include <stddef.h>

#include "object.h"

// Declare a user command.  The target must resolve to a method now, and the
// name must be an identifier that is not a reserved word, a root member or
// a built-in command.  Redeclaring a user command replaces it.  0 on
// success, -1 with a one-line message in `err`.
int shell_command_define(const char *name, const char *target, char *err, size_t err_size);

// Remove a user command.  0 on success, -1 with a message.
int shell_command_remove(const char *name, char *err, size_t err_size);

// Resolve `word` as a command whose target is a method right now.  Fills
// `out` (optional) with the method's node and `target` (optional,
// `target_size` bytes) with the target path.
bool shell_command_lookup(const char *word, node_t *out, char *target, size_t target_size);

// Visit every command in declaration order (built-ins first), resolved or
// not.  The callback returns false to stop.
typedef bool (*shell_command_fn)(const char *name, const char *target, bool builtin, void *ud);
void shell_command_each(shell_command_fn fn, void *ud);

// Drop the user commands (checkpoint restore, tests).
void shell_command_clear_user(void);

// The `shell.command` node: add / remove / list.
extern const class_desc_t shell_command_class;

#endif // GS_SHELL_COMMANDS_H
