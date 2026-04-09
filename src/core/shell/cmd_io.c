// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_io.c
// I/O stream setup for the command framework.
// Interactive mode: ctx->out/err point to stdout/stderr.
// Programmatic mode: ctx->out/err capture into buffers via fmemopen.

#include "cmd_io.h"

#include <string.h>

// Initialize I/O streams for a command invocation
void init_cmd_io(struct cmd_io *io, enum invoke_mode mode) {
    memset(io, 0, sizeof(*io));
    io->mode = mode;

    if (mode == INVOKE_INTERACTIVE) {
        io->out_stream = stdout;
        io->err_stream = stderr;
    } else {
        // Programmatic or pipe mode: capture into buffers using fmemopen
        io->out_buf[0] = '\0';
        io->err_buf[0] = '\0';
        io->out_len = 0;
        io->err_len = 0;

        // fmemopen opens a memory buffer as a FILE*
        io->out_stream = fmemopen(io->out_buf, CMD_OUT_BUF_SIZE, "w");
        io->err_stream = fmemopen(io->err_buf, CMD_ERR_BUF_SIZE, "w");

        // Disable buffering for immediate writes
        if (io->out_stream)
            setvbuf(io->out_stream, NULL, _IONBF, 0);
        if (io->err_stream)
            setvbuf(io->err_stream, NULL, _IONBF, 0);
    }
}

// Flush and finalize I/O streams. Attach captured output to result.
void finalize_cmd_io(struct cmd_io *io, struct cmd_result *res) {
    if (io->mode != INVOKE_INTERACTIVE) {
        if (io->out_stream && io->out_stream != stdout) {
            fflush(io->out_stream);
            // Get the current position as the output length
            io->out_len = (int)ftell(io->out_stream);
            fclose(io->out_stream);
            io->out_stream = NULL;
        }
        if (io->err_stream && io->err_stream != stderr) {
            fflush(io->err_stream);
            io->err_len = (int)ftell(io->err_stream);
            fclose(io->err_stream);
            io->err_stream = NULL;
        }

        // Null-terminate
        if (io->out_len >= 0 && io->out_len < CMD_OUT_BUF_SIZE)
            io->out_buf[io->out_len] = '\0';
        else
            io->out_buf[CMD_OUT_BUF_SIZE - 1] = '\0';

        if (io->err_len >= 0 && io->err_len < CMD_ERR_BUF_SIZE)
            io->err_buf[io->err_len] = '\0';
        else
            io->err_buf[CMD_ERR_BUF_SIZE - 1] = '\0';

        // Attach captured output to result
        res->output = io->out_buf;
        res->output_len = io->out_len;
        res->error_output = io->err_buf;
        res->error_len = io->err_len;
    }
}
