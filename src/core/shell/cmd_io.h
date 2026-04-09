// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_io.h
// I/O stream setup for the command framework.

#pragma once

#ifndef CMD_IO_H
#define CMD_IO_H

#include "cmd_types.h"

// I/O state for a command invocation
struct cmd_io {
    enum invoke_mode mode;

    char out_buf[CMD_OUT_BUF_SIZE];
    int out_len;
    char err_buf[CMD_ERR_BUF_SIZE];
    int err_len;

    // FILE* wrappers
    FILE *out_stream;
    FILE *err_stream;
};

// Initialize I/O streams for a command invocation
void init_cmd_io(struct cmd_io *io, enum invoke_mode mode);

// Flush and finalize I/O streams. Attach captured output to result.
void finalize_cmd_io(struct cmd_io *io, struct cmd_result *res);

#endif // CMD_IO_H
