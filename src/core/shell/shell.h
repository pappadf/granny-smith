// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell.h
// Public interface for command shell and command registration.

#pragma once

#ifndef SHELL_H
#define SHELL_H

// === Includes ===
#include "cmd_types.h"
#include "system.h"

#include <stddef.h>

// === Dispatch (Phase 5c — registry retired) ===
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

// In-place split of `line` into up to `max` argv tokens. The tokenizer
// honours quotes, backslash escapes, and `$(...)` expression tokens
// just like the legacy shell command line. `line` is mutated; argv
// pointers point inside it. Returns argc, or -1 on overflow.
int tokenize(char *line, char *argv[], int max);

// `log` legacy shell entry point (simple_fn signature). Exposed so the
// typed `log_set` wrapper can apply spec strings without going through
// `shell_dispatch`.
uint64_t cmd_log(int argc, char *argv[]);

// `set` legacy shell entry point (register/CC/memory writer).
uint64_t cmd_set(int argc, char *argv[]);

// === Tab Completion ===

void shell_tab_complete(const char *line, int cursor_pos, struct completion *out);

#endif // SHELL_H
