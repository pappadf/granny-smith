// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell.h
// Public interface for command shell and command registration.

#pragma once

#ifndef SHELL_H
#define SHELL_H

// === Includes ===
#include "cmd_complete.h" // struct completion
#include "system.h"

#include <stddef.h>

// === Dispatch ===
//
// There is no C entry point for a free-form line: every caller goes
// through the object model -- `object_eval("shell.run", [line])`, a script
// job, or the typed path form -- and the Shell class hands the line to
// the v2 script interpreter (script.h).

// The `shell` class (shell_class.c), attached by root_install.
struct class_desc;
extern const struct class_desc shell_class;

// Compose the current shell prompt into `buf`: "gs> " with no machine,
// "gs <model>> " while running, "gs <model> @<pc>> " when stopped. Used
// by the Shell class's `prompt` attribute and the headless REPL's
// `print_prompt`. Output is NUL-terminated and truncated to `buf_size - 1`.
void shell_build_prompt(char *buf, size_t buf_size);

// === Shell Lifecycle ===

// The shell's own setup (binding store, completion provider).  Called by
// core_init (core_init.h), which platforms call; idempotent.
int shell_init(void);

// Attach the `shell` node and its children (functions, alias, command)
// under the object root as cfg-scoped stubs.  root_install calls it on
// every install, like the other subsystems' install hooks.
struct config;
void shell_class_register(struct config *cfg);

// === Public command primitives (used by the typed object-model bridge) ===

// `cp` core: copy `src` (any VFS path: host, image, archive) to `dst`,
// which is always a HOST path in v1 -- hence the name.  Set `recursive`
// for directory copies.  On failure, fills `err_buf` (if non-NULL) with a
// human-readable message and returns a negative errno. Returns 0 on success.
int shell_cp_to_host(const char *src, const char *dst, bool recursive, char *err_buf, size_t err_cap);

// Copy everything inside `src` -- a directory, or an image or archive file,
// whose root is listed -- into host directory `dst` (created), with the
// resource fork and Finder info of each file as an AppleDouble "._" sidecar.
// The counts go to *files / *bytes (either may be NULL).  0 or a negated
// errno, with a message in err_buf.
int shell_cp_contents(const char *src, const char *dst, uint64_t *files, uint64_t *bytes, char *err_buf,
                      size_t err_cap);

// === Tab Completion ===
//
// shell_complete (cmd_complete.h) is the engine; `shell.complete` and
// `meta.complete` are its object-model surfaces.

#endif // SHELL_H
