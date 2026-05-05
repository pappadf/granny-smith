// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell_var.c
// Shell variable store + ${expr} substitution pass.
//
// `${...}` is the shell's one and only substitution sigil. The body is
// an expression evaluated by expr_eval — it can be a path (cpu.pc), a
// method call (memory.peek.l(0x100)), arithmetic (cpu.d0 + 4), or a
// bare identifier. Bare identifiers fall through to the bindings table
// which today is backed by the shell-variable store (`let` / WORK_DIR /
// TMP_DIR). Single quotes opt out of substitution entirely so deferred
// templates (logpoint messages) can carry literal `${...}` through to
// fire-time expansion.

#include "shell_var.h"

#include "alias.h"
#include "cmd_types.h"
#include "expr.h"
#include "object.h"
#include "shell.h"
#include "value.h"

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

/* --- bindings adapter ---------------------------------------------------- */

// expr_ctx_t.binding callback that exposes the shell-variable table.
// Returns V_NONE for unknown names so expr_eval falls through to its
// other resolution paths (path / alias / error).
static value_t shell_var_binding(void *ud, const char *name) {
    (void)ud;
    const char *v = shell_var_get(name);
    if (!v)
        return val_none();
    return val_str(v);
}

/* --- alias adapter ------------------------------------------------------- */
//
// expr_ctx_t.alias callback bound to the global alias table.

static const char *shell_alias_for_expr(void *ud, const char *name) {
    (void)ud;
    return alias_lookup(name, NULL);
}

/* --- ${expr} formatting -------------------------------------------------- */

// Format an evaluated value into a fresh malloc'd string suitable for
// splicing back into the source text (verbatim for V_STRING, hex for
// VAL_HEX numerics, "true"/"false" for V_BOOL, etc.). Mirrors the
// shell-side printer used for top-level evaluations.
static char *format_substitution(const value_t *v) {
    char buf[256];
    buf[0] = '\0';
    switch (v->kind) {
    case V_NONE:
        break;
    case V_BOOL:
        snprintf(buf, sizeof(buf), v->b ? "true" : "false");
        break;
    case V_INT:
        if (v->flags & VAL_HEX)
            snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uint64_t)v->i);
        else
            snprintf(buf, sizeof(buf), "%lld", (long long)v->i);
        break;
    case V_UINT:
        if (v->flags & VAL_HEX)
            snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v->u);
        else
            snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v->u);
        break;
    case V_FLOAT:
        snprintf(buf, sizeof(buf), "%g", v->f);
        break;
    case V_STRING:
        return strdup(v->s ? v->s : "");
    case V_BYTES: {
        char *out = (char *)malloc(3 + v->bytes.n * 2 + 1);
        if (!out)
            return NULL;
        char *q = out;
        *q++ = '0';
        *q++ = 'x';
        for (size_t i = 0; i < v->bytes.n; i++) {
            static const char hex[] = "0123456789abcdef";
            *q++ = hex[(v->bytes.p[i] >> 4) & 0xF];
            *q++ = hex[v->bytes.p[i] & 0xF];
        }
        *q = '\0';
        return out;
    }
    case V_ENUM:
        if (v->enm.table && (size_t)v->enm.idx < v->enm.n_table && v->enm.table[v->enm.idx])
            snprintf(buf, sizeof(buf), "%s", v->enm.table[v->enm.idx]);
        else
            snprintf(buf, sizeof(buf), "%d", v->enm.idx);
        break;
    case V_OBJECT:
        snprintf(buf, sizeof(buf), "<object>");
        break;
    case V_ERROR:
        snprintf(buf, sizeof(buf), "<error: %s>", v->err ? v->err : "");
        break;
    case V_LIST:
        snprintf(buf, sizeof(buf), "<list:%zu>", v->list.len);
        break;
    }
    return strdup(buf);
}

/* --- ${expr} substitution pass ------------------------------------------- */
//
// Walks `input` character by character, tracking single-quote state and
// escape sequences. Single-quoted regions are preserved verbatim
// (including any `${...}` they contain — that's the deferred-eval opt-
// out used by logpoint messages). Outside single quotes, `${...}` is
// parsed brace-balanced (with awareness of `"..."` and `'...'` strings
// in the body) and the contents are evaluated as an expression. The
// formatted result is spliced into the output stream.
//
// On evaluation error, the substitution pass writes a diagnostic to
// stderr and aborts — the shell wrapper treats this as a syntax error
// for the input line.

// Skip past a body-internal string literal so embedded `}`/`'`/`"` don't
// confuse the brace-depth tracker. Returns a pointer to the character
// past the closing quote (or to the terminating NUL on unterminated
// string). `quote` is either `'` or `"`.
static const char *skip_quoted(const char *p, char quote) {
    while (*p && *p != quote) {
        if (*p == '\\' && p[1])
            p += 2;
        else
            p++;
    }
    return *p ? p + 1 : p;
}

char *shell_var_expand(const char *input) {
    if (!input)
        return NULL;
    if (!strchr(input, '$'))
        return strdup(input);

    char *buf = malloc(MAX_EXPAND_LEN);
    if (!buf)
        return strdup(input);

    expr_ctx_t ectx = {
        .root = object_root(),
        .alias = shell_alias_for_expr,
        .alias_ud = NULL,
        .binding = shell_var_binding,
        .binding_ud = NULL,
    };

    size_t out = 0;
    const char *p = input;
    bool in_squote = false;

    while (*p && out < MAX_EXPAND_LEN - 1) {
        // Backslash escape: copy the next character literally regardless
        // of quote state. Lets users embed a literal `${` via `\${`.
        if (*p == '\\' && p[1]) {
            buf[out++] = *p++;
            if (out < MAX_EXPAND_LEN - 1)
                buf[out++] = *p++;
            continue;
        }
        if (*p == '\'') {
            in_squote = !in_squote;
            buf[out++] = *p++;
            continue;
        }
        if (!in_squote && p[0] == '$' && p[1] == '{') {
            // Find the matching `}`, respecting embedded strings and
            // nested `{...}` (e.g. method calls with braces in body —
            // unlikely in practice but keep the depth counter honest).
            const char *body = p + 2;
            const char *q = body;
            int depth = 1;
            while (*q && depth > 0) {
                if (*q == '\\' && q[1]) {
                    q += 2;
                    continue;
                }
                if (*q == '"' || *q == '\'') {
                    q = skip_quoted(q + 1, *q);
                    continue;
                }
                if (*q == '{')
                    depth++;
                else if (*q == '}')
                    depth--;
                if (depth == 0)
                    break;
                q++;
            }
            if (depth != 0) {
                fprintf(stderr, "shell: unterminated ${...}\n");
                free(buf);
                return NULL;
            }
            // Extract the body verbatim into a NUL-terminated copy.
            size_t blen = (size_t)(q - body);
            char *bcopy = (char *)malloc(blen + 1);
            if (!bcopy) {
                free(buf);
                return NULL;
            }
            memcpy(bcopy, body, blen);
            bcopy[blen] = '\0';

            value_t v = expr_eval(bcopy, &ectx);
            free(bcopy);
            if (v.kind == V_ERROR) {
                fprintf(stderr, "shell: ${...} error: %s\n", v.err ? v.err : "(unknown)");
                value_free(&v);
                free(buf);
                return NULL;
            }
            char *formatted = format_substitution(&v);
            value_free(&v);
            if (!formatted) {
                free(buf);
                return NULL;
            }
            size_t flen = strlen(formatted);
            if (out + flen >= MAX_EXPAND_LEN) {
                free(formatted);
                free(buf);
                fprintf(stderr, "shell: expansion exceeds %d bytes\n", MAX_EXPAND_LEN);
                return NULL;
            }
            memcpy(buf + out, formatted, flen);
            out += flen;
            free(formatted);
            p = q + 1; // skip past closing `}`
            continue;
        }

        // Regular character.
        buf[out++] = *p++;
    }

    buf[out] = '\0';
    return buf;
}

/* --- init ---------------------------------------------------------------- */

void shell_var_init(void) {
    shell_var_set("TMP_DIR", "tmp");
}
