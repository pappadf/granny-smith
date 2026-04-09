// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_parse.c
// Argument parser for the command framework.
// Parses tokenized argv[] into typed arg_value[] using the arg_spec declarations,
// including $-prefix symbol resolution.

#include "addr_format.h"
#include "cmd_symbol.h"
#include "cmd_types.h"
#include "memory.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Parse a boolean string
static bool parse_bool_str(const char *str, int *out) {
    if (strcasecmp(str, "on") == 0 || strcasecmp(str, "true") == 0 || strcmp(str, "1") == 0) {
        *out = 1;
        return true;
    }
    if (strcasecmp(str, "off") == 0 || strcasecmp(str, "false") == 0 || strcmp(str, "0") == 0) {
        *out = 0;
        return true;
    }
    return false;
}

// Parse an integer string (decimal, 0x hex, or $ hex)
static bool parse_int_str(const char *str, int64_t *out) {
    const char *s = str;
    if (*s == '$') {
        // Motorola hex prefix — try symbol resolution first
        struct resolved_symbol sym;
        if (resolve_symbol(s + 1, &sym)) {
            *out = (int64_t)sym.value;
            return true;
        }
        // Fall back to hex literal
        s++;
        char *end;
        *out = (int64_t)strtoll(s, &end, 16);
        return *end == '\0';
    }
    // Check for 0d prefix (explicit decimal)
    if (s[0] == '0' && (s[1] == 'd' || s[1] == 'D')) {
        char *end;
        *out = strtoll(s + 2, &end, 10);
        return *end == '\0';
    }
    // 0x prefix
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char *end;
        *out = (int64_t)strtoll(s, &end, 0);
        return *end == '\0';
    }
    // Try decimal first, then hex
    char *end;
    *out = strtoll(s, &end, 10);
    if (*end == '\0')
        return true;
    // Fall back to hex
    *out = (int64_t)strtoll(s, &end, 16);
    return *end == '\0';
}

// Parse an address string with $ symbol resolution
static bool parse_addr_str(const char *str, uint32_t *out) {
    // Try the existing parse_address which handles $reg, $hex, 0x, L:, P:, bare hex
    addr_space_t space;
    if (parse_address(str, out, &space))
        return true;

    // Also try symbol resolution for non-$ prefixed names (Mac globals)
    struct resolved_symbol sym;
    if (resolve_symbol(str, &sym)) {
        // For ARG_ADDR, a Mac global resolves to its address (not its value)
        if (sym.kind == SYM_MAC_GLOBAL)
            *out = sym.address;
        else
            *out = sym.value; // registers resolve to their value
        return true;
    }
    return false;
}

// Match a subcommand name (including aliases) against a token.
// Returns the matched subcmd_spec index, or -1.
static int match_subcmd(const struct subcmd_spec *subcmds, int n_subcmds, const char *token) {
    for (int i = 0; i < n_subcmds; i++) {
        if (subcmds[i].name == NULL)
            continue; // skip default entry
        if (strcasecmp(subcmds[i].name, token) == 0)
            return i;
        // Check aliases
        if (subcmds[i].aliases) {
            for (const char **a = subcmds[i].aliases; *a; a++) {
                if (strcasecmp(*a, token) == 0)
                    return i;
            }
        }
    }
    return -1;
}

// Find the default subcommand (name == NULL)
static int find_default_subcmd(const struct subcmd_spec *subcmds, int n_subcmds) {
    for (int i = 0; i < n_subcmds; i++) {
        if (subcmds[i].name == NULL)
            return i;
    }
    return -1;
}

// Parse a single argument token against its spec into arg_value
static bool parse_one_arg(const char *token, const struct arg_spec *spec, struct arg_value *val) {
    int base_type = ARG_BASE_TYPE(spec->type);
    val->type = base_type;
    val->present = 1;

    switch (base_type) {
    case ARG_STRING:
    case ARG_PATH:
        val->as_str = token;
        return true;

    case ARG_INT:
        return parse_int_str(token, &val->as_int);

    case ARG_ADDR: {
        // Handle $-prefixed symbol resolution for addresses
        if (token[0] == '$') {
            struct resolved_symbol sym;
            if (resolve_symbol(token + 1, &sym)) {
                if (sym.kind == SYM_MAC_GLOBAL)
                    val->as_addr = sym.address;
                else
                    val->as_addr = sym.value;
                return true;
            }
        }
        return parse_addr_str(token, &val->as_addr);
    }

    case ARG_BOOL:
        return parse_bool_str(token, &val->as_bool);

    case ARG_SYMBOL: {
        // Try $-prefixed resolution
        const char *name = token;
        if (name[0] == '$')
            name++;

        struct resolved_symbol sym;
        if (resolve_symbol(name, &sym)) {
            val->as_sym = sym;
            return true;
        }

        // Try as an address with size suffix (e.g., "0400.w")
        char buf[64];
        strncpy(buf, name, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *dot = strchr(buf, '.');
        if (dot) {
            *dot = '\0';
            const char *size_str = dot + 1;
            uint32_t addr;
            addr_space_t space;
            if (parse_address(buf, &addr, &space)) {
                val->as_sym.kind = SYM_UNKNOWN;
                val->as_sym.name = token;
                val->as_sym.address = addr;
                val->as_sym.size = 4;
                if (strcasecmp(size_str, "b") == 0)
                    val->as_sym.size = 1;
                else if (strcasecmp(size_str, "w") == 0)
                    val->as_sym.size = 2;
                else if (strcasecmp(size_str, "l") == 0)
                    val->as_sym.size = 4;
                // Read value from memory
                switch (val->as_sym.size) {
                case 1:
                    val->as_sym.value = memory_read_uint8(addr);
                    break;
                case 2:
                    val->as_sym.value = memory_read_uint16(addr);
                    break;
                default:
                    val->as_sym.value = memory_read_uint32(addr);
                    break;
                }
                return true;
            }
        }

        // Unresolved — still succeed so the handler can inspect raw_argv
        val->as_sym.kind = SYM_UNKNOWN;
        val->as_sym.name = token;
        return true;
    }

    case ARG_ENUM: {
        if (!spec->enum_values)
            return false;
        for (const char **ev = spec->enum_values; *ev; ev++) {
            if (strcasecmp(*ev, token) == 0) {
                val->as_str = *ev;
                return true;
            }
        }
        return false;
    }

    case ARG_REST:
        val->as_str = token;
        return true;
    }

    return false;
}

// Parse arguments for a command. Returns true on success.
// On failure, writes an error message to ctx->err and sets res->type = RES_ERR.
bool cmd_parse_args(int argc, char **argv, const struct cmd_reg *reg, struct cmd_context *ctx, struct cmd_result *res) {
    // Initialize context
    ctx->subcmd = NULL;
    ctx->nargs = 0;
    ctx->raw_argc = argc;
    ctx->raw_argv = argv;
    memset(ctx->args, 0, sizeof(ctx->args));

    int arg_idx = 1; // skip command name in argv[0]

    const struct arg_spec *specs = reg->args;
    int nspecs = reg->nargs;

    // Handle subcommands
    if (reg->subcmds && reg->n_subcmds > 0) {
        int matched = -1;

        // Check if argv[1] matches a subcommand
        if (arg_idx < argc) {
            matched = match_subcmd(reg->subcmds, reg->n_subcmds, argv[arg_idx]);
            if (matched >= 0) {
                ctx->subcmd = reg->subcmds[matched].name;
                specs = reg->subcmds[matched].args;
                nspecs = reg->subcmds[matched].nargs;
                arg_idx++;
            }
        }

        // If no subcommand matched, try the default
        if (matched < 0) {
            int def = find_default_subcmd(reg->subcmds, reg->n_subcmds);
            if (def >= 0) {
                ctx->subcmd = NULL; // default subcommand
                specs = reg->subcmds[def].args;
                nspecs = reg->subcmds[def].nargs;
            } else if (arg_idx < argc) {
                // No default and no match — error
                cmd_err(res, "unknown subcommand: %s", argv[arg_idx]);
                return false;
            }
        }
    }

    ctx->nargs = nspecs;

    // Parse positional arguments against specs
    for (int i = 0; i < nspecs && i < CMD_MAX_ARGS; i++) {
        if (arg_idx < argc) {
            // Handle ARG_REST: gather all remaining tokens
            if (ARG_BASE_TYPE(specs[i].type) == ARG_REST) {
                // Concatenate remaining args into a single string
                // Just use the first remaining arg for now (most uses are single string)
                ctx->args[i].type = ARG_REST;
                ctx->args[i].present = 1;
                // Build combined rest string (store pointer to argv for simplicity)
                ctx->args[i].as_str = argv[arg_idx];
                arg_idx = argc; // consume all remaining
                continue;
            }

            if (!parse_one_arg(argv[arg_idx], &specs[i], &ctx->args[i])) {
                cmd_err(res, "invalid %s: %s", specs[i].name, argv[arg_idx]);
                return false;
            }
            arg_idx++;
        } else if (ARG_IS_OPTIONAL(specs[i].type)) {
            ctx->args[i].present = 0;
            ctx->args[i].type = ARG_BASE_TYPE(specs[i].type);
        } else {
            cmd_err(res, "missing required argument: %s", specs[i].name);
            return false;
        }
    }

    return true;
}
