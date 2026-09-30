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

typedef struct script_func script_func_t;

// Longest command or function word, NUL included.  Longer words are neither.
#define SHELL_NAME_MAX 64

// What the first word of a statement means.  One answer for the
// interpreter, highlighting, completion and help, in the interpreter's
// order: a tree path, then a `def` function, then a command.
typedef enum {
    SHELL_HEAD_NONE,
    SHELL_HEAD_PATH,
    SHELL_HEAD_FUNCTION,
    SHELL_HEAD_COMMAND,
} shell_head_t;

// Resolve the bare word `word` (`len` bytes, no `.` or `[` after it).
//   SHELL_HEAD_PATH      `out` (optional) is its node
//   SHELL_HEAD_FUNCTION  `fn_out` (optional) takes a reference the caller
//                        releases with shell_func_release
//   SHELL_HEAD_COMMAND   `out` is the target method's node, `target`
//                        (optional, `target_size` bytes) its path
shell_head_t shell_head_resolve(const char *word, size_t len, node_t *out, script_func_t **fn_out, char *target,
                                size_t target_size);

// The same, skipping the path step, for a caller that already tried the
// word as a path.
shell_head_t shell_word_resolve(const char *word, size_t len, node_t *out, script_func_t **fn_out, char *target,
                                size_t target_size);

// Declare a user command.  The target must resolve to a method now (and be
// a path shorter than 256 bytes), and the name must be an identifier
// shorter than SHELL_NAME_MAX that is not a reserved word, a root member or
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

// Drop the user commands (tests).
void shell_command_clear_user(void);

// The `shell.command` node: add / remove / list.
extern const class_desc_t shell_command_class;

#endif // GS_SHELL_COMMANDS_H
