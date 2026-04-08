// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_parse.h
// Argument parser for the command framework.

#pragma once

#ifndef CMD_PARSE_H
#define CMD_PARSE_H

#include "cmd_types.h"

// Parse arguments for a command. Returns true on success.
// On failure, sets res->type = RES_ERR with an error message.
bool cmd_parse_args(int argc, char **argv, const struct cmd_reg *reg, struct cmd_context *ctx, struct cmd_result *res);

#endif // CMD_PARSE_H
