// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_eval.c
// Shell command `eval <path>` — additional entry point that resolves a
// path against the new object-model root and prints the result as
// JSON. Lives alongside the legacy shell during M2; the legacy shell
// remains primary until the M10 cutover.

#include "cmd_types.h"
#include "gs_api.h"
#include "shell.h"

#include <stdio.h>
#include <string.h>

#define EVAL_BUF_SIZE 4096

// `eval <path>` — resolve and print JSON. Returns 0 even on resolution
// failure: the JSON itself encodes the error, and the caller reads the
// stream. This matches `print` semantics — a printer, not a predicate.
static uint64_t cmd_eval(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: eval <path>\n");
        return 1;
    }
    char buf[EVAL_BUF_SIZE];
    (void)gs_eval(argv[1], NULL, buf, sizeof(buf));
    printf("%s\n", buf);
    return 0;
}

void cmd_eval_register(void);
void cmd_eval_register(void) {
    register_cmd("eval", "Object Model", "eval <path> - resolve a path and print the JSON value", cmd_eval);
}
