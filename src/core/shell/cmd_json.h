// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_json.h
// JSON serializer for cmd_result.

#pragma once

#ifndef CMD_JSON_H
#define CMD_JSON_H

#include "cmd_types.h"

// Serialize cmd_result to JSON for the JS bridge.
// Writes to buf (up to buflen bytes).
void cmd_result_to_json(const struct cmd_result *res, char *buf, int buflen);

#endif // CMD_JSON_H
