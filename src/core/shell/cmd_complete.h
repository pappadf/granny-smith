// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_complete.h
// Tab completion engine for the command framework.

#pragma once

#ifndef CMD_COMPLETE_H
#define CMD_COMPLETE_H

#include "cmd_types.h"

// Run tab completion for the given line at cursor_pos.
// Fills out->items with matching completions.
void shell_complete(const char *line, int cursor_pos, struct completion *out);

#endif // CMD_COMPLETE_H
