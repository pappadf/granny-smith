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

// A single shell variable.  The stored value is typed `value_t` so
// numeric assignments (e.g. `t0 = ${scheduler.host_user_ns}`) preserve
// their integer type through to subsequent arithmetic — no round-trip
// through strings.  Legacy --var FOO=BAR command-line flags and other
// string-only setters store V_STRING.
struct shell_var {
    char *name;
    value_t value;
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

// Set a typed shell variable (overwrites if it exists).  Takes
// ownership of `v` (frees the old value if any).
int shell_var_set_value(const char *name, value_t v) {
    if (!name || !name[0]) {
        value_free(&v);
        return -1;
    }

    int idx = find_var(name);
    if (idx >= 0) {
        // Overwrite existing — free the old typed value first.
        value_free(&vars[idx].value);
        vars[idx].value = v;
        return 0;
    }

    if (nvar >= MAX_VARS) {
        value_free(&v);
        return -1;
    }

    vars[nvar].name = strdup(name);
    if (!vars[nvar].name) {
        value_free(&v);
        return -1;
    }
    vars[nvar].value = v;
    nvar++;
    return 0;
}

// String setter — wraps the value as V_STRING.  Used by --var on the
// command line and by internal init code that has only strings.
int shell_var_set(const char *name, const char *value) {
    if (!value)
        return -1;
    return shell_var_set_value(name, val_str(value));
}

// Get a shell variable's typed value.  Returns V_NONE if undefined.
// The returned value is a borrowed view — do not free it.  Use
// value_dup() if you need to keep it past the next set/unset.
value_t shell_var_get_value(const char *name) {
    if (!name)
        return val_none();
    int idx = find_var(name);
    return (idx >= 0) ? vars[idx].value : val_none();
}

// Legacy string getter.  Returns a pointer into the value_t's storage
// if it's V_STRING, NULL otherwise.  New code should use
// shell_var_get_value().
const char *shell_var_get(const char *name) {
    if (!name)
        return NULL;
    int idx = find_var(name);
    if (idx < 0)
        return NULL;
    if (vars[idx].value.kind != V_STRING)
        return NULL;
    return vars[idx].value.s;
}

// Walk every variable in insertion order. Callers (Shell class's
// `vars` attribute, future scripting hooks) use this to enumerate the
// table without depending on the private storage layout.
void shell_var_each(shell_var_iter_fn fn, void *ud) {
    if (!fn)
        return;
    for (int i = 0; i < nvar; i++) {
        if (!fn(vars[i].name, &vars[i].value, ud))
            return;
    }
}

// Delete a shell variable (returns 0 on success, -1 if not found)
int shell_var_unset(const char *name) {
    int idx = find_var(name);
    if (idx < 0)
        return -1;

    free(vars[idx].name);
    value_free(&vars[idx].value);

    // move last entry into the gap
    if (idx < nvar - 1)
        vars[idx] = vars[nvar - 1];
    nvar--;
    return 0;
}

/* --- bindings adapter ---------------------------------------------------- */

// expr_ctx_t.binding callback that exposes the shell-variable table.
// Returns V_NONE for unknown names so expr_eval falls through to its
// other resolution paths (path / alias / error).  Hands back an owned
// duplicate so the caller can free it independently of our storage.
static value_t shell_var_binding(void *ud, const char *name) {
    (void)ud;
    value_t v = shell_var_get_value(name);
    if (v.kind == V_NONE)
        return v;
    return value_dup(&v);
}

/* --- alias adapter ------------------------------------------------------- */
//
// expr_ctx_t.alias callback bound to the global alias table.

static const char *shell_alias_for_expr(void *ud, const char *name) {
    (void)ud;
    return alias_lookup(name, NULL);
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

            // expr_substitute splits off any trailing `:fmt` format
            // spec at top level (proposal-shell-expressions §4.2.1) so
            // the colon does not leak into the expression parser, then
            // formats the result accordingly.
            char *err = NULL;
            char *formatted = expr_substitute(bcopy, &ectx, &err);
            free(bcopy);
            if (!formatted) {
                fprintf(stderr, "shell: ${...} error: %s\n", err ? err : "(unknown)");
                free(err);
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
