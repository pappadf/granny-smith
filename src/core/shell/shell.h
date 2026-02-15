// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell.h
// Public interface for command shell and command registration.

#pragma once

#ifndef SHELL_H
#define SHELL_H

// === Includes ===
#include "system.h"

// === Type Definitions ===
typedef uint64_t (*cmd_fn)(int argc, char *argv[]);

// === Operations ===

int register_cmd(const char *name, const char *category, const char *synopsis, cmd_fn fn);

int unregister_cmd(const char *name);

int shell_init(void);

uint64_t shell_dispatch(char *line);

uint64_t handle_command(const char *input_line);

void shell_interrupt(void);

#endif // SHELL_H
