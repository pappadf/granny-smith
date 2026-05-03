// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_io.c
// I/O stream setup for the command framework. Phase 5d retired
// captured-output mode; ctx->out / ctx->err are always stdout/stderr.

#include "cmd_io.h"

#include <string.h>

void init_cmd_io(struct cmd_io *io) {
    memset(io, 0, sizeof(*io));
    io->out_stream = stdout;
    io->err_stream = stderr;
}

void finalize_cmd_io(struct cmd_io *io, struct cmd_result *res) {
    (void)io;
    (void)res;
}
