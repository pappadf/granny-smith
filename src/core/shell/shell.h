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

// === Registration ===

// Register a command with full declarative metadata (args, subcommands, etc.)
int register_command(const struct cmd_reg *reg);

// Register a simple command (classic argc/argv signature, no declarative args).
// Convenience wrapper — creates a cmd_reg with simple_fn set.
int register_cmd(const char *name, const char *category, const char *synopsis, cmd_fn_simple fn);

// Unregister a command by name
int unregister_cmd(const char *name);

// === Dispatch ===

// Dispatch a command line with the given invocation mode.
// INVOKE_INTERACTIVE: output goes to stdout/stderr.
// INVOKE_PROGRAMMATIC: output is captured into res->output.
void dispatch_command(char *line, enum invoke_mode mode, struct cmd_result *res);

// Dispatch a command line interactively. Returns an integer result for callers
// that don't need the full cmd_result (e.g., headless script runner).
uint64_t shell_dispatch(char *line);

// === Shell Lifecycle ===

int shell_init(void);

void shell_interrupt(void);

// Borrowed pointer into the shared-memory shell-initialized flag.
// JS uses this to gate gsEval calls during the boot window, where
// ccall can otherwise fire before the worker has finished shell_init()
// and seen gs_classes_install_root().
volatile int32_t *gs_shell_ready_ptr(void);

// === JSON Bridge ===

// Get the JSON result buffer pointer (for WASM bridge)
char *get_cmd_json_result(void);

// === Tab Completion ===

void shell_tab_complete(const char *line, int cursor_pos, struct completion *out);

#endif // SHELL_H
