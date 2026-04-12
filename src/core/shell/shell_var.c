// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell_var.c
// Shell variable store, ${NAME} expansion, and the "var" command.

#include "shell_var.h"

#include "cmd_types.h"
#include "shell.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Maximum number of shell variables
#define MAX_VARS 64

// Maximum length of expanded output
#define MAX_EXPAND_LEN 4096

// A single shell variable
struct shell_var {
    char *name;
    char *value;
};

// Variable store (simple linear array)
static struct shell_var vars[MAX_VARS];
static int nvar = 0;

/* --- variable store ------------------------------------------------------ */

// Find a variable by name (returns index, or -1)
static int find_var(const char *name) {
    for (int i = 0; i < nvar; i++) {
        if (strcmp(vars[i].name, name) == 0)
            return i;
    }
    return -1;
}

// Set a shell variable (overwrites if it exists)
int shell_var_set(const char *name, const char *value) {
    if (!name || !name[0] || !value)
        return -1;

    int idx = find_var(name);
    if (idx >= 0) {
        // overwrite existing
        free(vars[idx].value);
        vars[idx].value = strdup(value);
        return vars[idx].value ? 0 : -1;
    }

    if (nvar >= MAX_VARS)
        return -1;

    // add new entry
    vars[nvar].name = strdup(name);
    vars[nvar].value = strdup(value);
    if (!vars[nvar].name || !vars[nvar].value) {
        free(vars[nvar].name);
        free(vars[nvar].value);
        return -1;
    }
    nvar++;
    return 0;
}

// Get a shell variable value (returns NULL if undefined)
const char *shell_var_get(const char *name) {
    if (!name)
        return NULL;
    int idx = find_var(name);
    return (idx >= 0) ? vars[idx].value : NULL;
}

// Delete a shell variable (returns 0 on success, -1 if not found)
int shell_var_unset(const char *name) {
    int idx = find_var(name);
    if (idx < 0)
        return -1;

    free(vars[idx].name);
    free(vars[idx].value);

    // move last entry into the gap
    if (idx < nvar - 1)
        vars[idx] = vars[nvar - 1];
    nvar--;
    return 0;
}

/* --- expansion ----------------------------------------------------------- */

// Check if c is a valid variable name character
static int is_var_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

// Expand ${NAME} references in a string.
// Returns a malloc'd copy with substitutions applied.
char *shell_var_expand(const char *input) {
    if (!input)
        return NULL;

    // quick check: if no '$' present, just return a copy
    if (!strchr(input, '$'))
        return strdup(input);

    char *buf = malloc(MAX_EXPAND_LEN);
    if (!buf)
        return strdup(input);

    size_t out = 0;
    const char *p = input;

    while (*p && out < MAX_EXPAND_LEN - 1) {
        if (p[0] == '$' && p[1] == '{') {
            // find closing brace
            const char *start = p + 2;
            const char *end = strchr(start, '}');
            if (end && end > start) {
                // extract variable name
                size_t namelen = (size_t)(end - start);
                char name[128];
                if (namelen < sizeof(name)) {
                    memcpy(name, start, namelen);
                    name[namelen] = '\0';

                    const char *val = shell_var_get(name);
                    if (val) {
                        size_t vlen = strlen(val);
                        if (out + vlen < MAX_EXPAND_LEN) {
                            memcpy(buf + out, val, vlen);
                            out += vlen;
                        }
                    }
                    // undefined vars expand to empty string
                    p = end + 1;
                    continue;
                }
            }
        }

        // regular character (or bare $ not followed by {)
        buf[out++] = *p++;
    }

    buf[out] = '\0';
    return buf;
}

/* --- "var" command -------------------------------------------------------- */

// var set NAME VALUE
static void cmd_var_set(struct cmd_context *ctx, struct cmd_result *res) {
    const char *name = ctx->args[0].as_str;
    const char *value = ctx->args[1].as_str;

    if (shell_var_set(name, value) < 0) {
        cmd_err(res, "failed to set variable (limit: %d)", MAX_VARS);
        return;
    }
    res->type = RES_OK;
}

// var unset NAME
static void cmd_var_unset(struct cmd_context *ctx, struct cmd_result *res) {
    const char *name = ctx->args[0].as_str;

    if (shell_var_unset(name) < 0) {
        cmd_err(res, "variable '%s' not defined", name);
        return;
    }
    res->type = RES_OK;
}

// var list (default subcommand)
static void cmd_var_list(struct cmd_context *ctx, struct cmd_result *res) {
    (void)ctx;
    if (nvar == 0) {
        fprintf(ctx->out, "(no variables defined)\n");
    } else {
        for (int i = 0; i < nvar; i++)
            fprintf(ctx->out, "%s=%s\n", vars[i].name, vars[i].value);
    }
    res->type = RES_OK;
}

// Unified command handler — dispatches to subcommands
static void cmd_var(struct cmd_context *ctx, struct cmd_result *res) {
    if (!ctx->subcmd)
        cmd_var_list(ctx, res);
    else if (strcmp(ctx->subcmd, "set") == 0)
        cmd_var_set(ctx, res);
    else if (strcmp(ctx->subcmd, "unset") == 0)
        cmd_var_unset(ctx, res);
}

// Argument specs for each subcommand
static const struct arg_spec var_set_args[] = {
    {"name",  ARG_STRING, "variable name"  },
    {"value", ARG_REST,   "value to assign"},
};

static const struct arg_spec var_unset_args[] = {
    {"name", ARG_STRING, "variable name"},
};

// Subcommand table
static const struct subcmd_spec var_subcmds[] = {
    {NULL,    NULL, NULL,           0, "list all variables"},
    {"set",   NULL, var_set_args,   2, "set a variable"    },
    {"unset", NULL, var_unset_args, 1, "unset a variable"  },
};

// Command registration record
static const struct cmd_reg var_cmd_reg = {
    .name = "var",
    .category = "General",
    .synopsis = "var [set NAME VALUE | unset NAME] - shell variables",
    .fn = cmd_var,
    .subcmds = var_subcmds,
    .n_subcmds = 3,
};

/* --- init ---------------------------------------------------------------- */

// Register the "var" command and seed built-in defaults
void shell_var_init(void) {
    register_command(&var_cmd_reg);

    // seed default variables
    shell_var_set("TMP_DIR", "tmp");
}
