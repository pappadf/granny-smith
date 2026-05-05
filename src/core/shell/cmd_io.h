// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_io.h
// I/O stream setup for the command framework.
//
// Phase 5d retired the captured-output mode (INVOKE_PROGRAMMATIC); all
// invocations now write straight to stdout/stderr. The struct and
// init/finalize helpers stay so the shell_*_argv apply functions in
// debug.c / system.c keep their existing call shape.

#pragma once

#ifndef CMD_IO_H
#define CMD_IO_H

#include "cmd_types.h"

// I/O state for a command invocation. The active fields are the
// stdout/stderr stream handles; the buffer fields are kept zero-init
// for any holdover callers that might touch them.
struct cmd_io {
    char out_buf[CMD_OUT_BUF_SIZE];
    int out_len;
    char err_buf[CMD_ERR_BUF_SIZE];
    int err_len;

    FILE *out_stream;
    FILE *err_stream;
};

// Initialise I/O streams; out_stream → stdout, err_stream → stderr.
void init_cmd_io(struct cmd_io *io);

// No-op kept for symmetry with init_cmd_io; call sites still invoke it.
void finalize_cmd_io(struct cmd_io *io, struct cmd_result *res);

#endif // CMD_IO_H
