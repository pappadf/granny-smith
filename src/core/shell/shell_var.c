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
                        p = end + 1;
                        continue;
                    }
                    // Unknown name — leave the ${...} text intact so
                    // downstream consumers (the object-model
                    // string-interpolator inside logpoint messages,
                    // assert messages, etc.) can handle it. Pre-M5
                    // behavior dropped unresolved names silently.
                }
            }
        }

        // regular character (or bare $ not followed by {)
        buf[out++] = *p++;
    }

    buf[out] = '\0';
    return buf;
}

/* --- init ---------------------------------------------------------------- */

// Phase 5c — legacy `var` shell command registration retired. The
// `${VAR}` expansion in shell_var_expand() is still active. Tests
// that need to set variables interactively use `let NAME=value` (TBD)
// or pre-set via the platform wrapper.
void shell_var_init(void) {
    shell_var_set("TMP_DIR", "tmp");
}
