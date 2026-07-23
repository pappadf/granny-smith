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
// Free-form dispatch is gone; only the typed path-form remains. JS
// callers use gs_eval() directly — including for free-form lines,
// which go through `gs_eval("shell.run", [line])` (see
// proposal-shell-as-object-model-citizen.md). The headless REPL uses
// shell_dispatch() directly; the Shell class's `run` method calls the
// file-static `dispatch_command()` in shell.c.

uint64_t shell_dispatch(char *line);

// Compose the current shell prompt (e.g. "<disasm> > " when a machine
// is up, "gs> " otherwise) into `buf`. Used by the Shell class's
// `prompt` attribute and the headless REPL's `print_prompt`. Output is
// NUL-terminated and truncated to `buf_size - 1`.
void shell_build_prompt(char *buf, size_t buf_size);

// === Shell Lifecycle ===

int shell_init(void);

// === Public command primitives (used by the typed object-model bridge) ===

// `cp` core: copy `src` to `dst`. Set `recursive` for directory copies.
// On failure, fills `err_buf` (if non-NULL) with a human-readable message
// and returns a negative errno. Returns 0 on success.
int shell_cp(const char *src, const char *dst, bool recursive, char *err_buf, size_t err_cap);

// === Tab Completion ===

void shell_tab_complete(const char *line, int cursor_pos, struct completion *out);

#endif // SHELL_H
