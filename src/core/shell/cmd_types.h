// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_types.h
// Shell command framework types: declarative args, structured results,
// captured I/O, tab completion, and symbol resolution.

#pragma once

#ifndef CMD_TYPES_H
#define CMD_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// Maximum number of positional arguments per command or subcommand
#define CMD_MAX_ARGS 8

// Maximum number of subcommands per command
#define CMD_MAX_SUBCMDS 16

// Output capture buffer sizes
#define CMD_OUT_BUF_SIZE 8192
#define CMD_ERR_BUF_SIZE 2048

// Result string scratch buffer size
#define CMD_RESULT_BUF_SIZE 256

// Tab completion maximum items
#define CMD_MAX_COMPLETIONS 64

// === Argument Types ===

// Argument types recognized by the framework
enum arg_type {
    ARG_STRING = 0, // passed as-is (char *)
    ARG_INT = 1, // parsed as int64_t (decimal or 0x hex)
    ARG_ADDR = 2, // parsed as uint32_t address (hex, symbol, or expression)
    ARG_BOOL = 3, // "on"/"off"/"true"/"false"/"1"/"0"
    ARG_SYMBOL = 4, // Mac global or register name (resolved to name + address)
    ARG_PATH = 5, // filesystem path (enables path completion)
    ARG_ENUM = 6, // one of a fixed set of string values
    ARG_REST = 7, // remaining argv[] gathered into a string
};

// Flag: argument is optional (OR'd with arg_type)
#define ARG_OPTIONAL 0x100

// Extract the base type (strip optional flag)
#define ARG_BASE_TYPE(t) ((t) & 0xFF)

// Check if an argument is optional
#define ARG_IS_OPTIONAL(t) (((t) & ARG_OPTIONAL) != 0)

// === Symbol Resolution ===

// Kind of resolved symbol
enum symbol_kind {
    SYM_REGISTER,
    SYM_MAC_GLOBAL,
    SYM_UNKNOWN,
};

// Result of symbol resolution
struct resolved_symbol {
    const char *name; // canonical name (e.g. "PC", "MBState")
    uint32_t address; // resolved address
    uint32_t value; // current value at that address
    int size; // data size in bytes (1/2/4)
    enum symbol_kind kind;
};

// === Argument Specification ===

// Describe one positional argument
struct arg_spec {
    const char *name; // e.g. "address", "count"
    int type; // arg_type | ARG_OPTIONAL
    const char *description; // for help text
    const char **enum_values; // for ARG_ENUM: NULL-terminated list of valid values
};

// Describe one subcommand variant
struct subcmd_spec {
    const char *name; // e.g. "del", "clear", "list"; NULL = default
    const char **aliases; // NULL-terminated list of aliases (e.g. {"r", NULL})
    const struct arg_spec *args; // argument specs for this subcommand
    int nargs;
    const char *description; // one-line description for help
};

// === Parsed Argument Block ===

// A single parsed argument value
struct arg_value {
    int type; // arg_type
    int present; // 0 if optional and not provided
    union {
        int64_t as_int;
        uint32_t as_addr;
        int as_bool;
        const char *as_str;
        struct resolved_symbol as_sym;
    };
};

// Invocation mode (set by the dispatcher)
enum invoke_mode {
    INVOKE_INTERACTIVE, // user typed at shell prompt
    INVOKE_PROGRAMMATIC, // called from JS via runCommand()
    INVOKE_PIPE, // output feeds into another command (future)
};

// cmd_io is defined in cmd_io.h (not here, to avoid redefinition)

// The full parsed context handed to a command
struct cmd_context {
    const char *subcmd; // matched subcommand name (NULL if none)
    struct arg_value args[CMD_MAX_ARGS]; // parsed positional arguments
    int nargs; // number of argument specs
    int raw_argc; // original argc (escape hatch)
    char **raw_argv; // original argv (escape hatch)

    // I/O streams
    FILE *out; // where command output goes
    FILE *err; // where error messages go
};

// === Result Type ===

enum result_type {
    RES_OK, // success, no data payload
    RES_INT, // integer data
    RES_STR, // string data
    RES_BOOL, // boolean
    RES_ERR, // error with message
};

struct cmd_result {
    enum result_type type;
    union {
        int64_t as_int;
        const char *as_str; // points to static or result_buf
        int as_bool;
    };
    char result_buf[CMD_RESULT_BUF_SIZE]; // scratch space for formatted string results

    // Captured I/O (populated by dispatcher for programmatic calls)
    const char *output; // captured stdout text (NULL if interactive)
    int output_len;
    const char *error_output; // captured stderr text
    int error_len;
};

// === Tab Completion ===

// Completion result
struct completion {
    const char *items[CMD_MAX_COMPLETIONS];
    int count;
};

// Completion callback (optional per-command)
typedef void (*complete_fn)(struct cmd_context *ctx, const char *partial, struct completion *out);

// === Command Function Signatures ===

// New-style command handler: receives parsed args and returns a structured result
typedef void (*cmd_fn)(struct cmd_context *ctx, struct cmd_result *res);

// Simple command handler: classic (argc, argv) → uint64_t signature.
// Commands using this signature are wrapped automatically by the dispatcher.
typedef uint64_t (*cmd_fn_simple)(int argc, char *argv[]);

// === Command Registration ===

struct cmd_reg {
    const char *name;
    const char **aliases; // NULL-terminated list (e.g. {"b", NULL})
    const char *category;
    const char *synopsis; // one-line description

    cmd_fn fn; // new-style handler (NULL if using simple_fn)
    cmd_fn_simple simple_fn; // simple handler (NULL if using fn)
    complete_fn complete; // optional custom completer (NULL = auto only)

    const struct arg_spec *args; // positional args (for commands without subcmds)
    int nargs;
    const struct subcmd_spec *subcmds; // subcommand variants (NULL if none)
    int n_subcmds;
};

// === Convenience Macros ===

// Print to command output stream
#define cmd_printf(ctx, ...) fprintf((ctx)->out, __VA_ARGS__)

// Print to command error stream
#define cmd_eprintf(ctx, ...) fprintf((ctx)->err, __VA_ARGS__)

// Set result to OK
#define cmd_ok(res)                                                                                                    \
    do {                                                                                                               \
        (res)->type = RES_OK;                                                                                          \
    } while (0)

// Set result to integer
#define cmd_int(res, v)                                                                                                \
    do {                                                                                                               \
        (res)->type = RES_INT;                                                                                         \
        (res)->as_int = (v);                                                                                           \
    } while (0)

// Set result to boolean
#define cmd_bool(res, v)                                                                                               \
    do {                                                                                                               \
        (res)->type = RES_BOOL;                                                                                        \
        (res)->as_bool = (v);                                                                                          \
    } while (0)

// Set result to error (uses result_buf)
#define cmd_err(res, ...)                                                                                              \
    do {                                                                                                               \
        (res)->type = RES_ERR;                                                                                         \
        snprintf((res)->result_buf, CMD_RESULT_BUF_SIZE, __VA_ARGS__);                                                 \
        (res)->as_str = (res)->result_buf;                                                                             \
    } while (0)

#endif // CMD_TYPES_H
