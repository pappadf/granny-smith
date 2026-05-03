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
// callers use gs_eval() / gs_inspect() directly. The legacy
// shell_dispatch() and dispatch_command() entry points stay so the
// headless REPL and the WASM `g_cmd_pending` queue keep working — they
// route every line through the typed path-form parser internally.

void dispatch_command(char *line, enum invoke_mode mode, struct cmd_result *res);
uint64_t shell_dispatch(char *line);

// === Shell Lifecycle ===

int shell_init(void);

void shell_interrupt(void);

// Borrowed pointer into the shared-memory shell-initialized flag.
// JS uses this to gate gsEval calls during the boot window, where
// ccall can otherwise fire before the worker has finished shell_init()
// and seen gs_classes_install_root().
volatile int32_t *gs_shell_ready_ptr(void);

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

// === JSON Bridge ===

// Get the JSON result buffer pointer (for WASM bridge)
char *get_cmd_json_result(void);

// === Tab Completion ===

void shell_tab_complete(const char *line, int cursor_pos, struct completion *out);

#endif // SHELL_H
