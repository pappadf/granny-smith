// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_complete.c
// Tab completion engine for the command framework.
// Provides metadata-driven completion for commands, subcommands, symbols,
// enums, booleans, and paths, plus optional custom completers.

#include "cmd_complete.h"
#include "cmd_symbol.h"

#include <ctype.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>

// Command registry node (defined in shell.c)
struct cmd_reg_node {
    struct cmd_reg reg;
    struct cmd_reg_node *next;
};

// Head of the command registry (defined in shell.c)
extern struct cmd_reg_node *cmd_head;

// Complete filesystem paths matching a prefix
static void complete_paths(const char *prefix, struct completion *out) {
    // Split prefix into directory and partial filename
    const char *last_slash = strrchr(prefix, '/');
    char dir[256] = ".";
    const char *partial = prefix;

    if (last_slash) {
        size_t dir_len = (size_t)(last_slash - prefix);
        if (dir_len == 0) {
            dir[0] = '/';
            dir[1] = '\0';
        } else {
            if (dir_len >= sizeof(dir))
                dir_len = sizeof(dir) - 1;
            memcpy(dir, prefix, dir_len);
            dir[dir_len] = '\0';
        }
        partial = last_slash + 1;
    }

    size_t plen = strlen(partial);
    DIR *d = opendir(dir);
    if (!d)
        return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && out->count < CMD_MAX_COMPLETIONS) {
        if (ent->d_name[0] == '.' && partial[0] != '.')
            continue; // skip hidden files unless prefix starts with .
        if (plen == 0 || strncmp(ent->d_name, partial, plen) == 0)
            out->items[out->count++] = ent->d_name;
    }
    closedir(d);
}

// Complete enum values
static void complete_enum(const char **enum_values, const char *partial, struct completion *out) {
    if (!enum_values)
        return;
    size_t plen = strlen(partial);
    for (const char **ev = enum_values; *ev; ev++) {
        if (strncasecmp(*ev, partial, plen) == 0 && out->count < CMD_MAX_COMPLETIONS)
            out->items[out->count++] = *ev;
    }
}

// Complete boolean values
static void complete_bool(const char *partial, struct completion *out) {
    static const char *bool_values[] = {"on", "off", "true", "false", NULL};
    size_t plen = strlen(partial);
    for (const char **v = bool_values; *v; v++) {
        if (strncasecmp(*v, partial, plen) == 0 && out->count < CMD_MAX_COMPLETIONS)
            out->items[out->count++] = *v;
    }
}

// Main completion engine
void shell_complete(const char *line, int cursor_pos, struct completion *out) {
    if (!line || !out)
        return;
    out->count = 0;

    // Extract the token being completed (word at cursor position)
    // Simple approach: split by spaces, find which token cursor is in
    char buf[512];
    int len = cursor_pos;
    if (len >= (int)sizeof(buf))
        len = (int)sizeof(buf) - 1;
    memcpy(buf, line, len);
    buf[len] = '\0';

    // Split into tokens
    char *tokens[32];
    int ntokens = 0;
    char *p = buf;
    while (*p && ntokens < 32) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        tokens[ntokens++] = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        if (*p)
            *p++ = '\0';
    }

    // If line ends with space, we're completing a new (empty) token
    int completing_new = (len > 0 && isspace((unsigned char)line[len - 1]));
    const char *partial = completing_new ? "" : (ntokens > 0 ? tokens[ntokens - 1] : "");
    int token_idx = completing_new ? ntokens : ntokens - 1;

    // Token 0: complete command names
    if (token_idx == 0) {
        size_t plen = strlen(partial);
        for (struct cmd_reg_node *n = cmd_head; n; n = n->next) {
            if (strncasecmp(n->reg.name, partial, plen) == 0 && out->count < CMD_MAX_COMPLETIONS)
                out->items[out->count++] = n->reg.name;
            // Also check aliases
            if (n->reg.aliases) {
                for (const char **a = n->reg.aliases; *a; a++) {
                    if (strncasecmp(*a, partial, plen) == 0 && out->count < CMD_MAX_COMPLETIONS)
                        out->items[out->count++] = *a;
                }
            }
        }
        return;
    }

    // Find the command (token 0)
    const struct cmd_reg *reg = NULL;
    for (struct cmd_reg_node *n = cmd_head; n; n = n->next) {
        if (strcasecmp(n->reg.name, tokens[0]) == 0) {
            reg = &n->reg;
            break;
        }
        if (n->reg.aliases) {
            for (const char **a = n->reg.aliases; *a; a++) {
                if (strcasecmp(*a, tokens[0]) == 0) {
                    reg = &n->reg;
                    break;
                }
            }
            if (reg)
                break;
        }
    }
    if (!reg)
        return;

    // Determine arg specs (handling subcommands)
    const struct arg_spec *specs = reg->args;
    int nspecs = reg->nargs;
    int arg_start = 1; // first token that's an argument

    if (reg->subcmds && reg->n_subcmds > 0) {
        // Token 1: try to match or complete subcommand
        if (token_idx == 1) {
            size_t plen = strlen(partial);
            for (int i = 0; i < reg->n_subcmds; i++) {
                if (!reg->subcmds[i].name)
                    continue;
                if (strncasecmp(reg->subcmds[i].name, partial, plen) == 0 && out->count < CMD_MAX_COMPLETIONS)
                    out->items[out->count++] = reg->subcmds[i].name;
                if (reg->subcmds[i].aliases) {
                    for (const char **a = reg->subcmds[i].aliases; *a; a++) {
                        if (strncasecmp(*a, partial, plen) == 0 && out->count < CMD_MAX_COMPLETIONS)
                            out->items[out->count++] = *a;
                    }
                }
            }
            // Also allow completing as if default subcommand
            // Fall through to arg completion below
            int def = -1;
            for (int i = 0; i < reg->n_subcmds; i++) {
                if (!reg->subcmds[i].name) {
                    def = i;
                    break;
                }
            }
            if (def >= 0) {
                specs = reg->subcmds[def].args;
                nspecs = reg->subcmds[def].nargs;
            }
            // If we already have subcommand completions, return
            if (out->count > 0)
                return;
        }

        // Try to identify which subcommand was used
        if (ntokens > 1) {
            for (int i = 0; i < reg->n_subcmds; i++) {
                if (!reg->subcmds[i].name)
                    continue;
                if (strcasecmp(reg->subcmds[i].name, tokens[1]) == 0) {
                    specs = reg->subcmds[i].args;
                    nspecs = reg->subcmds[i].nargs;
                    arg_start = 2;
                    break;
                }
                if (reg->subcmds[i].aliases) {
                    for (const char **a = reg->subcmds[i].aliases; *a; a++) {
                        if (strcasecmp(*a, tokens[1]) == 0) {
                            specs = reg->subcmds[i].args;
                            nspecs = reg->subcmds[i].nargs;
                            arg_start = 2;
                            break;
                        }
                    }
                    if (arg_start == 2)
                        break;
                }
            }
        }
    }

    // Complete argument at the current position
    int arg_pos = token_idx - arg_start;
    if (arg_pos < 0 || arg_pos >= nspecs) {
        // Try custom completer
        if (reg->complete) {
            struct cmd_context ctx = {0};
            reg->complete(&ctx, partial, out);
        }
        return;
    }

    const struct arg_spec *spec = &specs[arg_pos];
    int base_type = ARG_BASE_TYPE(spec->type);

    switch (base_type) {
    case ARG_SYMBOL:
    case ARG_ADDR:
        // If partial starts with $, complete symbols
        if (partial[0] == '$') {
            complete_symbols(partial + 1, out);
        } else if (base_type == ARG_SYMBOL) {
            complete_symbols(partial, out);
        }
        break;

    case ARG_ENUM:
        complete_enum(spec->enum_values, partial, out);
        break;

    case ARG_BOOL:
        complete_bool(partial, out);
        break;

    case ARG_PATH:
        complete_paths(partial, out);
        break;

    default:
        break;
    }

    // Try custom completer as well
    if (reg->complete) {
        struct cmd_context ctx = {0};
        reg->complete(&ctx, partial, out);
    }
}
