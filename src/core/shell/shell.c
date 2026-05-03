// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell.c
// Interactive command shell for emulator debugging and control.

#include "shell.h"

#include "alias.h"
#include "cmd_complete.h"
#include "cmd_io.h"
#include "cmd_parse.h"
#include "cmd_types.h"
#include "expr.h"
#include "gs_thread.h"
#include "log.h"
#include "object.h"
#include "parse.h"
#include "shell_var.h"
#include "value.h"
#include "vfs.h"

#include <inttypes.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Volatile so the shared-memory pointer exposed to JS sees the real
// flag flip — avoids the optimiser caching the constant `0` it sees
// at init time and confusing the JS-side gsEval ready check.
static volatile int32_t shell_initialized = 0;

// Borrowed pointer into the shared-memory shell-initialized flag. JS
// polls this on first gsEval call before issuing the ccall — without
// it, calls fired during the boot window between Module-ready and the
// worker leaving shell_init() see the stale empty root class.
volatile int32_t *gs_shell_ready_ptr(void) {
    return &shell_initialized;
}

// Phase 5c — legacy command registry deleted. The typed object-model
// bridge is the sole dispatch surface; shell_dispatch falls straight
// through to the path-form parser.

/* --- tokenizer ----------------------------------------------------------- */
#define MAXTOK 32

// In-place tokenizer exposed via shell.h as `shell_tokenize` so the
// typed object-model bridge can split free-form spec strings (e.g.
// logpoint specs, log argv) without routing through `shell_dispatch`.
// `line` is mutated in place; returned argv pointers point inside it.
int tokenize(char *line, char *argv[], int max) {
    // Tokenizer with support for: \-escapes, ASCII and UTF-8 curly
    // quotes, and `$(...)` expression tokens (proposal §4.1.2). Inside
    // a `$(...)` token, whitespace and `[...]` are part of the token
    // until the matching `)` closes it. Quote state is reset at the
    // expression boundary; the expression parser owns its own
    // tokenization inside.
    int argc = 0;
    int esc = 0;
    enum { Q_NONE = 0, Q_DQUOTE, Q_SQUOTE, Q_CURLY } qstate = Q_NONE;

    char *p = line;
    while (*p) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (argc == max)
            return -1;

        argv[argc++] = p;
        char *dst = p;

        for (;;) {
            unsigned char c = (unsigned char)*p;
            if (c == '\0') {
                *dst = '\0';
                break;
            }

            if (esc) {
                *dst++ = *p++;
                esc = 0;
                continue;
            }

            // UTF-8 curly quotes (E2 80 9C/9D)
            if ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x80 &&
                ((unsigned char)p[2] == 0x9C || (unsigned char)p[2] == 0x9D)) {
                if (qstate == Q_NONE)
                    qstate = Q_CURLY;
                else if (qstate == Q_CURLY)
                    qstate = Q_NONE;
                else {
                    *dst++ = *p++;
                    continue;
                }
                p += 3;
                continue;
            }

            if (*p == '\\') {
                esc = 1;
                p++;
                continue;
            }

            // Open `$(` outside any quote — start an expression token.
            // Inside the expression we count `(`/`)` for nesting but
            // honor `"..."` strings (with backslash escapes) so a `)`
            // inside a string literal does not close the expression.
            if (qstate == Q_NONE && p[0] == '$' && p[1] == '(') {
                int paren_depth = 1;
                *dst++ = *p++; // copy '$'
                *dst++ = *p++; // copy '('
                bool in_str = false;
                while (*p && paren_depth > 0) {
                    char ch = *p;
                    if (in_str) {
                        if (ch == '\\' && p[1]) {
                            *dst++ = *p++;
                            *dst++ = *p++;
                            continue;
                        }
                        if (ch == '"')
                            in_str = false;
                        *dst++ = *p++;
                        continue;
                    }
                    if (ch == '"') {
                        in_str = true;
                        *dst++ = *p++;
                        continue;
                    }
                    if (ch == '(')
                        paren_depth++;
                    else if (ch == ')')
                        paren_depth--;
                    *dst++ = *p++;
                }
                // paren_depth==0 means we copied the closing `)`. If
                // we hit end of line first, the expression is
                // unterminated and the expr parser will report it
                // when this token is evaluated.
                continue;
            }

            if (*p == '"') {
                if (qstate == Q_NONE)
                    qstate = Q_DQUOTE;
                else if (qstate == Q_DQUOTE)
                    qstate = Q_NONE;
                else
                    *dst++ = *p;
                p++;
                continue;
            }

            if (*p == '\'') {
                if (qstate == Q_NONE)
                    qstate = Q_SQUOTE;
                else if (qstate == Q_SQUOTE)
                    qstate = Q_NONE;
                else
                    *dst++ = *p;
                p++;
                continue;
            }

            if (qstate == Q_NONE && isspace((unsigned char)*p)) {
                *dst = '\0';
                p++;
                break;
            }

            *dst++ = *p++;
        }
    }
    return argc;
}

/* --- $(...) expansion + operator check ----------------------------------- */
//
// After tokenize() splits the line into argv[], any token of the form
// `$(...)` is the body of an expression that the user wants
// substituted. We hand the body to expr_eval (proposal §4) and replace
// the token with the formatted result.
//
// Top-level operators (proposal §4.1.3): a token that is exactly an
// operator string at depth zero is a syntax error with a corrective
// message. This catches `cpu.d0 = $cpu.pc + 4` (silently dropping
// "+ 4") which is the failure mode the proposal calls out.

static const char *shell_alias_resolver(void *ud, const char *name) {
    (void)ud;
    return alias_lookup(name, NULL);
}

// Returns true if `tok` is exactly an operator that's only legal
// inside `$(...)`. Multi-char tokens like `-1` or `cpu.d*` are not
// flagged because they aren't an operator alone.
static bool is_bare_operator(const char *tok) {
    if (!tok || !*tok)
        return false;
    static const char *const ops[] = {
        "+", "-",  "*",  "/",  "%",  "&",  "|",  "^",  "~",  "!",  "<",
        ">", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", NULL,
    };
    for (int i = 0; ops[i]; i++)
        if (strcmp(tok, ops[i]) == 0)
            return true;
    return false;
}

// Format a value_t into a fresh malloc'd string suitable for splicing
// back into argv[]. Numerics in hex; bools as "true"/"false"; strings
// verbatim; bytes as a 0x... hex run.
static char *format_value_for_shell(const value_t *v) {
    char buf[256];
    buf[0] = '\0';
    switch (v->kind) {
    case V_NONE:
        snprintf(buf, sizeof(buf), "");
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
        snprintf(buf, sizeof(buf), "<object:%s>", object_class(v->obj) ? object_class(v->obj)->name : "?");
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

// Walk argv[]; for every token starting with `$(` evaluate the
// expression body and replace argv[i] with the formatted result.
// Tokens that are bare top-level operators trigger a syntax error.
//
// `replaced[i]` points at malloc'd storage when argv[i] was rewritten;
// the caller frees the array entries after dispatch.
//
// Returns 0 on success, -1 if the line should be rejected (errors
// already emitted to stderr).
static int expand_args(int argc, char **argv, char **replaced) {
    expr_ctx_t ectx = {
        .root = object_root(),
        .alias = shell_alias_resolver,
        .alias_ud = NULL,
    };
    // argv[0] is the command name; never expand it.
    for (int i = 1; i < argc; i++) {
        char *tok = argv[i];
        if (!tok)
            continue;

        // Operator at top level → corrective syntax error.
        if (is_bare_operator(tok)) {
            fprintf(stderr,
                    "shell: '%s' is an operator and is only valid inside $(...);"
                    " write `$(... %s ...)` instead\n",
                    tok, tok);
            return -1;
        }

        if (tok[0] != '$' || tok[1] != '(')
            continue;

        // Strip the surrounding `$(` and `)`. The closing paren may be
        // missing if the user typed an unterminated expression — the
        // expr parser will reject it cleanly in that case.
        size_t len = strlen(tok);
        size_t bend = len;
        if (bend >= 1 && tok[bend - 1] == ')')
            bend--;
        size_t bstart = 2;
        char saved = tok[bend];
        tok[bend] = '\0';
        const char *body = tok + bstart;

        value_t v = expr_eval(body, &ectx);
        tok[bend] = saved;

        if (val_is_error(&v)) {
            fprintf(stderr, "shell: $(...) error: %s\n", v.err ? v.err : "(unknown)");
            value_free(&v);
            return -1;
        }
        char *formatted = format_value_for_shell(&v);
        value_free(&v);
        if (!formatted) {
            fputs("shell: out of memory expanding $(...)\n", stderr);
            return -1;
        }
        replaced[i] = formatted;
        argv[i] = formatted;
    }
    return 0;
}

static void free_replacements(int argc, char **replaced) {
    for (int i = 0; i < argc; i++) {
        if (replaced[i]) {
            free(replaced[i]);
            replaced[i] = NULL;
        }
    }
}

// Phase 5c — legacy `help` / `echo` / `time` / `add` / `remove`
// shell built-ins retired. `echo` and other utilities are typed root
// methods now (gs_classes.c).

// Phase 5c — legacy filesystem commands (`ls`, `cd`, `mkdir`, `mv`,
// `cat`, `exists`, `size`, `rm`) and the registry-based execute_cmd
// retired. Typed object-model methods (vfs.ls, vfs.mkdir, vfs.cat,
// path_exists, path_size, …) replace them.

// Forward declaration — definition lives below in the shell-form
// grammar block. Returns 0 on success, -1 on error, 1 if unhandled.
static int try_path_dispatch(int argc, char **argv);

// Dispatch a command line. Phase 5c — the legacy registry is gone;
// everything routes through the typed path-form parser.
void dispatch_command(char *line, struct cmd_result *res) {
    memset(res, 0, sizeof(*res));
    res->type = RES_OK;

    if (!shell_initialized) {
        cmd_err(res, "shell not initialized");
        return;
    }

    char *expanded = shell_var_expand(line);
    char *to_parse = expanded ? expanded : line;

    char *argv[MAXTOK];
    int argc = tokenize(to_parse, argv, MAXTOK);
    if (argc <= 0) {
        free(expanded);
        return;
    }

    char *replaced[MAXTOK] = {0};
    if (expand_args(argc, argv, replaced) < 0) {
        cmd_err(res, "syntax error");
        free_replacements(MAXTOK, replaced);
        free(expanded);
        return;
    }

    int pd = try_path_dispatch(argc, argv);
    if (pd < 0)
        cmd_err(res, "command failed");
    // pd > 0 (not a recognised path) → silent no-op, matching legacy parity.
    free_replacements(MAXTOK, replaced);
    free(expanded);
}

// === New shell-form grammar dispatch (proposal §4.1) =======================
//
// Falls between the legacy command lookup and the "unknown command"
// suggestion: if argv[0] resolves as a path against the object root,
// dispatch as one of:
//   - bare path (argc == 1)            → node_get + print
//   - path = value (argv[1] == "=")    → node_set with parsed argv[2]
//   - path arg arg arg (M_METHOD)      → node_call with parsed args
// Returns true if the line was handled (caller should not fall through
// to suggest_command), false otherwise.

static void format_value_print(const value_t *v) {
    if (!v)
        return;
    switch (v->kind) {
    case V_NONE:
        break;
    case V_BOOL:
        printf("%s\n", v->b ? "true" : "false");
        break;
    case V_INT:
        printf("%" PRId64 "\n", v->i);
        break;
    case V_UINT:
        if (v->flags & VAL_HEX)
            printf("0x%" PRIx64 "\n", v->u);
        else
            printf("%" PRIu64 "\n", v->u);
        break;
    case V_FLOAT:
        printf("%g\n", v->f);
        break;
    case V_STRING:
        printf("%s\n", v->s ? v->s : "");
        break;
    case V_BYTES:
        printf("0x");
        for (size_t i = 0; i < v->bytes.n; i++)
            printf("%02x", v->bytes.p[i]);
        printf("\n");
        break;
    case V_ENUM:
        if (v->enm.table && (size_t)v->enm.idx < v->enm.n_table && v->enm.table[v->enm.idx])
            printf("%s\n", v->enm.table[v->enm.idx]);
        else
            printf("enum:%d\n", v->enm.idx);
        break;
    case V_LIST:
        // Expand list elements inline: [item1, item2, ...]. Strings are
        // quoted, ints/uints printed in their natural base, objects use
        // <class:name>, nested lists recurse via the size form.
        printf("[");
        for (size_t i = 0; i < v->list.len; i++) {
            const value_t *e = &v->list.items[i];
            if (i)
                printf(", ");
            switch (e->kind) {
            case V_NONE:
                printf("null");
                break;
            case V_BOOL:
                printf("%s", e->b ? "true" : "false");
                break;
            case V_INT:
                printf("%" PRId64, e->i);
                break;
            case V_UINT:
                if (e->flags & VAL_HEX)
                    printf("0x%" PRIx64, e->u);
                else
                    printf("%" PRIu64, e->u);
                break;
            case V_FLOAT:
                printf("%g", e->f);
                break;
            case V_STRING:
                printf("\"%s\"", e->s ? e->s : "");
                break;
            case V_BYTES:
                printf("<bytes:%zu>", e->bytes.n);
                break;
            case V_LIST:
                printf("<list:%zu>", e->list.len);
                break;
            case V_ENUM:
                if (e->enm.table && (size_t)e->enm.idx < e->enm.n_table && e->enm.table[e->enm.idx])
                    printf("\"%s\"", e->enm.table[e->enm.idx]);
                else
                    printf("enum:%d", e->enm.idx);
                break;
            case V_OBJECT: {
                const class_desc_t *cc = e->obj ? object_class(e->obj) : NULL;
                printf("<%s:%s>", cc && cc->name ? cc->name : "object",
                       e->obj && object_name(e->obj) ? object_name(e->obj) : "");
                break;
            }
            case V_ERROR:
                printf("<error>");
                break;
            }
        }
        printf("]\n");
        break;
    case V_OBJECT: {
        const class_desc_t *cls = v->obj ? object_class(v->obj) : NULL;
        const char *cls_name = (cls && cls->name) ? cls->name : "object";
        const char *o_name = v->obj ? object_name(v->obj) : NULL;
        printf("<%s:%s>\n", cls_name, o_name ? o_name : "");
        break;
    }
    case V_ERROR:
        fprintf(stderr, "%s\n", v->err ? v->err : "(error)");
        break;
    }
}

// Returns 0 if handled successfully, -1 if handled with error, or 1 if
// not a path (caller continues with the unknown-command path).
static int try_path_dispatch(int argc, char **argv) {
    if (argc < 1)
        return 1;
    // Quick rejection: a path either contains '.' / '[' or matches a
    // root member name. The cheaper test (look for '.' or '[') covers
    // the common case and lets the legacy registry win on names like
    // `run` that ARE root methods but whose legacy command aliases
    // matter for back-compat with scripts.
    bool dotted = strchr(argv[0], '.') || strchr(argv[0], '[');
    node_t n = object_resolve(object_root(), argv[0]);
    if (!node_valid(n) || (!dotted && (!n.member || n.member->kind != M_METHOD)))
        return 1;

    // Setter form: `path = value`.
    if (argc >= 3 && strcmp(argv[1], "=") == 0) {
        if (!n.member || n.member->kind != M_ATTR) {
            fprintf(stderr, "'%s' is not a settable attribute\n", argv[0]);
            return -1;
        }
        value_t v = parse_literal_full(argv[2], NULL, 0);
        if (val_is_error(&v)) {
            // Fall back to treating the token as a string literal —
            // mirrors the method-call branch and lets attribute setters
            // accept opaque tokens (ISO timestamps, paths, identifiers
            // that aren't valid number/bool/enum literals).
            value_free(&v);
            v = val_str(argv[2]);
        }
        value_t result = node_set(n, v);
        if (val_is_error(&result)) {
            fprintf(stderr, "set %s: %s\n", argv[0], result.err ? result.err : "failed");
            value_free(&result);
            return -1;
        }
        value_free(&result);
        return 0;
    }

    // Method call form: `path arg arg arg ...`.
    if (n.member && n.member->kind == M_METHOD) {
        int call_argc = argc - 1;
        value_t *vals = call_argc > 0 ? (value_t *)calloc((size_t)call_argc, sizeof(value_t)) : NULL;
        for (int i = 0; i < call_argc; i++) {
            vals[i] = parse_literal_full(argv[i + 1], NULL, 0);
            if (val_is_error(&vals[i])) {
                // Fall back to treating the token as a string literal —
                // mirrors the way most legacy commands accept a bare
                // word as a path/name.
                value_free(&vals[i]);
                vals[i] = val_str(argv[i + 1]);
            }
        }
        value_t result = node_call(n, call_argc, vals);
        for (int i = 0; i < call_argc; i++)
            value_free(&vals[i]);
        free(vals);
        if (val_is_error(&result)) {
            fprintf(stderr, "%s: %s\n", argv[0], result.err ? result.err : "call failed");
            value_free(&result);
            return -1;
        }
        format_value_print(&result);
        value_free(&result);
        return 0;
    }

    // Bare path read.
    if (argc == 1) {
        value_t v = node_get(n);
        if (val_is_error(&v)) {
            // Print the error to stderr but return 0 — matches the
            // legacy `eval` semantics. Tests intentionally probe empty
            // indexed slots etc. ("scsi.devices[5]") and expect the
            // script to keep going. Use `assert (exists(path))` for
            // strict membership checks.
            fprintf(stderr, "%s: %s\n", argv[0], v.err ? v.err : "read failed");
            value_free(&v);
            return 0;
        }
        format_value_print(&v);
        value_free(&v);
        return 0;
    }

    // Anything else (path with extra tokens that isn't a setter or a
    // method call) is ambiguous — let the legacy code path handle it.
    return 1;
}

// Dispatch interactively and return integer result. Phase 5c — only
// the typed path-form parser remains.
uint64_t shell_dispatch(char *line) {
    if (!shell_initialized)
        return -1;

    gs_thread_assert_worker("shell_dispatch");

    char *expanded = shell_var_expand(line);
    char *to_parse = expanded ? expanded : line;

    char *argv[MAXTOK];
    int argc = tokenize(to_parse, argv, MAXTOK);
    if (argc < 0) {
        fputs("too many arguments\n", stderr);
        free(expanded);
        return 0;
    }
    if (argc == 0) {
        free(expanded);
        return 0;
    }

    char *replaced[MAXTOK] = {0};
    if (expand_args(argc, argv, replaced) < 0) {
        free_replacements(MAXTOK, replaced);
        free(expanded);
        return (uint64_t)-1;
    }

    int pd = try_path_dispatch(argc, argv);
    free_replacements(MAXTOK, replaced);
    free(expanded);
    // pd == 0 → handled successfully
    // pd  < 0 → handled with error
    // pd  > 0 → not a recognised path; treat as no-op so the script keeps
    //          going (legacy parity — `path_size("...")` and other expression
    //          forms used to silently no-op when invoked at top level).
    return (pd < 0) ? (uint64_t)-1 : 0;
}

// Tab completion entry point
void shell_tab_complete(const char *line, int cursor_pos, struct completion *out) {
    shell_complete(line, cursor_pos, out);
}

/* --- shell init ---------------------------------------------------------- */
int shell_init(void) {
    if (shell_initialized)
        return 0;

    log_init();
    shell_var_init();

    // Install the top-level object-root methods (assert, echo, cp,
    // peeler, rom_probe, …) so JS callers (`gsEval`) and the typed
    // path-form parser can reach them.
    extern void gs_classes_install_root(void);
    gs_classes_install_root();

    // Latch the worker pthread for the thread-affinity guard. From now
    // on (under MODE=debug/sanitize) any call into shell_dispatch() or
    // gs_eval() from a different thread aborts with GS_ASSERTF.
    gs_thread_record_worker();

    shell_initialized = 1;
    return 0;
}
