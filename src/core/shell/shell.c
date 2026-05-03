// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell.c
// Interactive command shell for emulator debugging and control.

#include "shell.h"

#include "alias.h"
#include "cmd_complete.h"
#include "cmd_io.h"
#include "cmd_json.h"
#include "cmd_parse.h"
#include "cmd_types.h"
#include "expr.h"
#include "gs_thread.h"
#include "log.h"
#include "object.h"
#include "parse.h"
#include "peeler_shell.h"
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

// JSON result buffer for WASM bridge (16KB)
#define CMD_JSON_BUF_SIZE 16384
static char g_cmd_json_buffer[CMD_JSON_BUF_SIZE];

/* --- command registry ---------------------------------------------------- */

// Single unified command registry node
struct cmd_reg_node {
    struct cmd_reg reg;
    struct cmd_reg_node *next;
};

// Head of the command registry (exported for the completion engine)
struct cmd_reg_node *cmd_head = NULL;

// Find a command by name or alias
static struct cmd_reg_node *find_cmd(const char *name) {
    for (struct cmd_reg_node *n = cmd_head; n; n = n->next) {
        if (strcasecmp(n->reg.name, name) == 0)
            return n;
        if (n->reg.aliases) {
            for (const char **a = n->reg.aliases; *a; a++) {
                if (strcasecmp(*a, name) == 0)
                    return n;
            }
        }
    }
    return NULL;
}

// Register a command with full declarative metadata
int register_command(const struct cmd_reg *reg) {
    if (!reg || !reg->name || (!reg->fn && !reg->simple_fn))
        return -1;
    if (find_cmd(reg->name))
        return -1; // already registered

    struct cmd_reg_node *node = malloc(sizeof(struct cmd_reg_node));
    if (!node)
        return -1;

    node->reg = *reg; // shallow copy (all strings are static)
    node->next = cmd_head;
    cmd_head = node;
    return 0;
}

// Register a simple command (classic argc/argv signature)
int register_cmd(const char *name, const char *category, const char *synopsis, cmd_fn_simple fn) {
    if (find_cmd(name))
        return -1;
    return register_command(&(struct cmd_reg){
        .name = name,
        .category = category,
        .synopsis = synopsis,
        .simple_fn = fn,
    });
}

// Unregister a command by name
int unregister_cmd(const char *name) {
    struct cmd_reg_node *prev = NULL, *cur = cmd_head;
    while (cur) {
        if (strcmp(cur->reg.name, name) == 0) {
            if (prev)
                prev->next = cur->next;
            else
                cmd_head = cur->next;
            free(cur);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    return -1;
}

/* --- "did you mean?" suggestion ------------------------------------------ */

// Simple edit distance (Levenshtein) for short strings
static int edit_distance(const char *a, const char *b) {
    int la = strlen(a), lb = strlen(b);
    if (la > 20 || lb > 20)
        return 99;
    int dp[21][21];
    for (int i = 0; i <= la; i++)
        dp[i][0] = i;
    for (int j = 0; j <= lb; j++)
        dp[0][j] = j;
    for (int i = 1; i <= la; i++) {
        for (int j = 1; j <= lb; j++) {
            int cost = (tolower((unsigned char)a[i - 1]) != tolower((unsigned char)b[j - 1])) ? 1 : 0;
            int del = dp[i - 1][j] + 1;
            int ins = dp[i][j - 1] + 1;
            int sub = dp[i - 1][j - 1] + cost;
            dp[i][j] = del < ins ? (del < sub ? del : sub) : (ins < sub ? ins : sub);
        }
    }
    return dp[la][lb];
}

// Suggest closest matching command or alias for an unknown command name
static void suggest_command(const char *name) {
    const char *best = NULL;
    int best_dist = 4; // max distance threshold

    for (struct cmd_reg_node *n = cmd_head; n; n = n->next) {
        int d = edit_distance(name, n->reg.name);
        if (d < best_dist) {
            best_dist = d;
            best = n->reg.name;
        }
        if (n->reg.aliases) {
            for (const char **a = n->reg.aliases; *a; a++) {
                d = edit_distance(name, *a);
                if (d < best_dist) {
                    best_dist = d;
                    best = *a;
                }
            }
        }
    }

    if (best)
        fprintf(stderr, "Unknown command \"%s\". Did you mean \"%s\"?\n", name, best);
    else
        fprintf(stderr, "Unknown command \"%s\". Type \"help\" for a list of commands.\n", name);
}

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

/* --- built-in commands ---------------------------------------------------- */

// Help category display order
static const char *help_category_order[] = {"Execution",  "Breakpoints", "Inspection",    "Tracing",       "Display",
                                            "Input",      "Media",       "Configuration", "Checkpointing", "Scheduler",
                                            "Filesystem", "Archive",     "Testing",       "Logging",       "AppleTalk",
                                            "General",    NULL};

// Print commands in a given category
static void help_print_category(const char *cat) {
    int printed = 0;
    for (struct cmd_reg_node *n = cmd_head; n; n = n->next) {
        if (strcmp(n->reg.category, cat) == 0) {
            if (!printed) {
                printf("\n%s:\n", cat);
                printed = 1;
            }
            printf("  %-14s %s\n", n->reg.name, n->reg.synopsis);
        }
    }
}

static uint64_t cmd_help(int argc, char *argv[]) {
    if (argc == 1) {
        // Print categories in defined order
        for (int i = 0; help_category_order[i]; i++)
            help_print_category(help_category_order[i]);

        // Print any remaining categories not in the fixed order
        for (struct cmd_reg_node *n = cmd_head; n; n = n->next) {
            int found = 0;
            for (int i = 0; help_category_order[i]; i++) {
                if (strcmp(n->reg.category, help_category_order[i]) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                int already = 0;
                for (struct cmd_reg_node *prev = cmd_head; prev != n; prev = prev->next) {
                    if (strcmp(prev->reg.category, n->reg.category) == 0) {
                        already = 1;
                        break;
                    }
                }
                if (!already)
                    help_print_category(n->reg.category);
            }
        }
    } else {
        // Per-command help
        for (int i = 1; i < argc; ++i) {
            struct cmd_reg_node *c = find_cmd(argv[i]);
            if (!c) {
                printf("Unknown command \"%s\"\n", argv[i]);
                continue;
            }

            printf("%s — %s\n", c->reg.name, c->reg.synopsis);

            // Print subcommands if any
            if (c->reg.subcmds && c->reg.n_subcmds > 0) {
                printf("\n");
                for (int j = 0; j < c->reg.n_subcmds; j++) {
                    const struct subcmd_spec *sc = &c->reg.subcmds[j];
                    if (!sc->name) {
                        if (sc->nargs > 0) {
                            printf("  %s", c->reg.name);
                            for (int k = 0; k < sc->nargs; k++) {
                                if (ARG_IS_OPTIONAL(sc->args[k].type))
                                    printf(" [%s]", sc->args[k].name);
                                else
                                    printf(" <%s>", sc->args[k].name);
                            }
                            if (sc->description)
                                printf("   %s", sc->description);
                            printf("\n");
                        }
                    } else {
                        printf("  %s %s", c->reg.name, sc->name);
                        if (sc->aliases) {
                            printf(" (");
                            for (const char **a = sc->aliases; *a; a++) {
                                if (a != sc->aliases)
                                    printf(", ");
                                printf("%s", *a);
                            }
                            printf(")");
                        }
                        for (int k = 0; k < sc->nargs; k++) {
                            if (ARG_IS_OPTIONAL(sc->args[k].type))
                                printf(" [%s]", sc->args[k].name);
                            else
                                printf(" <%s>", sc->args[k].name);
                        }
                        if (sc->description)
                            printf("   %s", sc->description);
                        printf("\n");
                    }
                }
            } else if (c->reg.args && c->reg.nargs > 0) {
                printf("\n  %s", c->reg.name);
                for (int k = 0; k < c->reg.nargs; k++) {
                    if (ARG_IS_OPTIONAL(c->reg.args[k].type))
                        printf(" [%s]", c->reg.args[k].name);
                    else
                        printf(" <%s>", c->reg.args[k].name);
                }
                printf("\n");
            }

            // Print aliases
            if (c->reg.aliases) {
                printf("  Aliases:");
                for (const char **a = c->reg.aliases; *a; a++)
                    printf(" %s", *a);
                printf("\n");
            }
        }
    }
    return 0;
}

static uint64_t cmd_echo(int argc, char *argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (i > 1)
            putchar(' ');
        fputs(argv[i], stdout);
    }
    putchar('\n');
    return 0;
}

/* Demo of a command that can be loaded and unloaded at run-time */
static uint64_t cmd_time(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    time_t t = time(NULL);
    printf("Current time: %s", ctime(&t));
    return 0;
}

static uint64_t cmd_add(int argc, char *argv[]) {
    if (argc != 2) {
        puts("usage: add time");
        return 0;
    }
    if (strcmp(argv[1], "time") == 0) {
        if (register_cmd("time", "General", "time  – show the clock", cmd_time) == 0)
            puts("time command added");
        else
            puts("time command already present");
    } else
        puts("only \"time\" is available in this demo");
    return 0;
}

static uint64_t cmd_remove(int argc, char *argv[]) {
    if (argc != 2) {
        puts("usage: remove time");
        return 0;
    }
    if (strcmp(argv[1], "time") == 0) {
        if (unregister_cmd("time") == 0)
            puts("time command removed");
        else
            puts("time command is not loaded");
    } else
        puts("only \"time\" is available in this demo");
    return 0;
}

/* --- file system commands ------------------------------------------------ */
// FS commands route through the VFS layer (src/core/vfs/).  In Phase 1
// there is only one backend (host), so behaviour is byte-identical to the
// pre-refactor libc calls.  Phase 2 adds an image backend plus an
// auto-mount resolver; the call sites here do not change.

// ls [dir] — list directory contents; defaults to current_dir
static void cmd_ls(struct cmd_context *ctx, struct cmd_result *res) {
    const char *path = ctx->args[0].present ? ctx->args[0].as_str : vfs_get_cwd();
    vfs_dir_t *dir = NULL;
    const vfs_backend_t *be = NULL;
    int rc = vfs_opendir(path, &dir, &be);
    if (rc < 0) {
        cmd_printf(ctx, "ls: cannot open directory '%s': %s\n", path, strerror(-rc));
        cmd_ok(res);
        return;
    }
    vfs_dirent_t entry;
    int r;
    while ((r = be->readdir(dir, &entry)) > 0)
        cmd_printf(ctx, "%s\n", entry.name);
    be->closedir(dir);
    cmd_ok(res);
}

// cd <dir> — change current directory.  Honours the "bare image path =
// descend" rule (§2.9) so `cd foo.img` enters the partition-list root.
// The logical cwd is tracked in the VFS layer; we only call libc chdir()
// when the destination is a host path (image paths have no real host
// cwd, so subsequent relative paths re-resolve via normalise_path).
static void cmd_cd(struct cmd_context *ctx, struct cmd_result *res) {
    const char *input = ctx->args[0].as_str;
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *bctx = NULL;
    const char *tail = NULL;
    int rc = vfs_resolve_descend(input, resolved, sizeof(resolved), &be, &bctx, &tail);
    if (rc < 0) {
        cmd_printf(ctx, "cd: cannot change to '%s': %s\n", input, strerror(-rc));
        cmd_ok(res);
        return;
    }
    vfs_stat_t st = {0};
    rc = be->stat(bctx, tail, &st);
    if (rc < 0) {
        cmd_printf(ctx, "cd: cannot change to '%s': %s\n", input, strerror(-rc));
        cmd_ok(res);
        return;
    }
    if (!(st.mode & VFS_MODE_DIR)) {
        cmd_printf(ctx, "cd: not a directory: %s\n", input);
        cmd_ok(res);
        return;
    }
    // For host-backed directories keep the process cwd in sync so libc
    // calls elsewhere (e.g. non-VFS consumers) see the same working dir.
    // Image-backed paths have no host equivalent, so skip chdir() and
    // rely on the logical cwd tracked by the VFS layer.
    if (strcmp(be->scheme, "host") == 0) {
        if (chdir(resolved) != 0) {
            cmd_printf(ctx, "cd: cannot change to '%s': %s\n", input, strerror(errno));
            cmd_ok(res);
            return;
        }
    }
    vfs_set_cwd(resolved);
    cmd_printf(ctx, "Changed directory to %s\n", vfs_get_cwd());
    cmd_ok(res);
}

// mkdir <dir> — create a directory
static void cmd_mkdir(struct cmd_context *ctx, struct cmd_result *res) {
    const char *dir = ctx->args[0].as_str;
    int rc = vfs_mkdir(dir);
    if (rc == 0)
        cmd_printf(ctx, "Directory '%s' created\n", dir);
    else
        cmd_printf(ctx, "mkdir: cannot create directory '%s': %s\n", dir, strerror(-rc));
    cmd_ok(res);
}

// mv <src> <dst> — rename/move a file or directory
static void cmd_mv(struct cmd_context *ctx, struct cmd_result *res) {
    const char *src = ctx->args[0].as_str;
    const char *dst = ctx->args[1].as_str;
    int rc = vfs_rename(src, dst);
    if (rc == 0)
        cmd_printf(ctx, "Moved '%s' to '%s'\n", src, dst);
    else
        cmd_printf(ctx, "mv: cannot move '%s' to '%s': %s\n", src, dst, strerror(-rc));
    cmd_ok(res);
}

// cat <path> — stream file contents to the command output
static void cmd_cat(struct cmd_context *ctx, struct cmd_result *res) {
    const char *path = ctx->args[0].as_str;
    vfs_file_t *f = NULL;
    const vfs_backend_t *be = NULL;
    int rc = vfs_open(path, &f, &be);
    if (rc < 0) {
        cmd_printf(ctx, "cat: cannot open '%s': %s\n", path, strerror(-rc));
        cmd_int(res, 1);
        return;
    }
    // Stream bytes in 4KB chunks; pread handles the offset tracking.
    uint8_t buf[4096];
    uint64_t off = 0;
    for (;;) {
        size_t n = 0;
        rc = be->read(f, off, buf, sizeof(buf), &n);
        if (rc < 0) {
            cmd_printf(ctx, "cat: read error on '%s': %s\n", path, strerror(-rc));
            be->close(f);
            cmd_int(res, 1);
            return;
        }
        if (n == 0)
            break;
        fwrite(buf, 1, n, ctx->out);
        off += n;
    }
    be->close(f);
    cmd_int(res, 0);
}

// exists <path> — exit code 0 if the path exists, 1 otherwise
static void cmd_exists(struct cmd_context *ctx, struct cmd_result *res) {
    const char *path = ctx->args[0].as_str;
    vfs_stat_t st;
    cmd_int(res, (vfs_stat(path, &st) == 0) ? 0 : 1);
}

// size <path> — return file size in bytes (0 on stat failure)
static void cmd_size(struct cmd_context *ctx, struct cmd_result *res) {
    const char *path = ctx->args[0].as_str;
    vfs_stat_t st = {0};
    int rc = vfs_stat(path, &st);
    if (rc < 0) {
        cmd_printf(ctx, "size: cannot stat '%s': %s\n", path, strerror(-rc));
        cmd_int(res, 0);
        return;
    }
    cmd_int(res, (int64_t)st.size);
}

// rm <path> — unlink a file
static void cmd_rm(struct cmd_context *ctx, struct cmd_result *res) {
    const char *path = ctx->args[0].as_str;
    int rc = vfs_unlink(path);
    if (rc < 0) {
        cmd_printf(ctx, "rm: cannot remove '%s': %s\n", path, strerror(-rc));
        cmd_int(res, 1);
        return;
    }
    cmd_int(res, 0);
}

// Argument specs for filesystem commands (ARG_PATH drives tab completion)
static const struct arg_spec fs_ls_args[] = {
    {"dir", ARG_PATH | ARG_OPTIONAL, "directory to list"},
};
static const struct arg_spec fs_dir_args[] = {
    {"dir", ARG_PATH, "directory"},
};
static const struct arg_spec fs_mv_args[] = {
    {"src", ARG_PATH, "source path"     },
    {"dst", ARG_PATH, "destination path"},
};
static const struct arg_spec fs_path_args[] = {
    {"path", ARG_PATH, "file path"},
};

/* --- dispatcher ---------------------------------------------------------- */

// Execute a command through a cmd_reg_node (handles both fn and simple_fn)
static void execute_cmd(struct cmd_reg_node *node, int argc, char **argv, enum invoke_mode mode,
                        struct cmd_result *res) {
    if (node->reg.fn) {
        // Full command handler with parsed args
        struct cmd_io io;
        init_cmd_io(&io, mode);

        struct cmd_context ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.out = io.out_stream;
        ctx.err = io.err_stream;

        if (cmd_parse_args(argc, argv, &node->reg, &ctx, res))
            node->reg.fn(&ctx, res);

        finalize_cmd_io(&io, res);
    } else if (node->reg.simple_fn) {
        // Simple (argc, argv) → uint64_t handler
        uint64_t retval = node->reg.simple_fn(argc, argv);
        res->type = RES_INT;
        res->as_int = (int64_t)retval;
    }
}

// Dispatch a command line with the given invocation mode
void dispatch_command(char *line, enum invoke_mode mode, struct cmd_result *res) {
    memset(res, 0, sizeof(*res));
    res->type = RES_OK;

    if (!shell_initialized) {
        cmd_err(res, "shell not initialized");
        return;
    }

    // expand ${VAR} references before tokenizing
    char *expanded = shell_var_expand(line);
    char *to_parse = expanded ? expanded : line;

    char *argv[MAXTOK];
    int argc = tokenize(to_parse, argv, MAXTOK);
    if (argc <= 0)
        return;

    char *replaced[MAXTOK] = {0};
    if (expand_args(argc, argv, replaced) < 0) {
        cmd_err(res, "syntax error");
        free_replacements(MAXTOK, replaced);
        free(expanded);
        return;
    }

    struct cmd_reg_node *c = find_cmd(argv[0]);
    if (c) {
        execute_cmd(c, argc, argv, mode, res);
        free_replacements(MAXTOK, replaced);
        free(expanded);
        return;
    }

    // Unknown command: print suggestion, but return OK (exit code 0)
    // to match the convention that unknown commands are not fatal errors.
    suggest_command(argv[0]);
    res->type = RES_OK;
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

// Dispatch interactively and return integer result
uint64_t shell_dispatch(char *line) {
    if (!shell_initialized)
        return -1;

    // Thread-affinity guard (compiled out in release). See gs_thread.h.
    gs_thread_assert_worker("shell_dispatch");

    // expand ${VAR} references before tokenizing
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

    struct cmd_reg_node *c = find_cmd(argv[0]);
    if (!c) {
        // No legacy-registry hit — try the new shell-form grammar
        // (proposal §4.1): bare path read, `path = value` setter, or
        // `path arg arg arg` method call.
        int pd = try_path_dispatch(argc, argv);
        if (pd <= 0) {
            free_replacements(MAXTOK, replaced);
            free(expanded);
            return pd == 0 ? 0 : (uint64_t)-1;
        }
        suggest_command(argv[0]);
        free_replacements(MAXTOK, replaced);
        free(expanded);
        return 0;
    }

    struct cmd_result res;
    memset(&res, 0, sizeof(res));
    execute_cmd(c, argc, argv, INVOKE_INTERACTIVE, &res);
    free_replacements(MAXTOK, replaced);
    free(expanded);

    if (res.type == RES_INT)
        return (uint64_t)res.as_int;
    if (res.type == RES_BOOL)
        return (uint64_t)res.as_bool;
    if (res.type == RES_ERR) {
        if (res.as_str)
            fprintf(stderr, "%s\n", res.as_str);
        return (uint64_t)-1;
    }
    return 0;
}

// Get the JSON result buffer pointer
char *get_cmd_json_result(void) {
    return g_cmd_json_buffer;
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
    peeler_shell_init();
    shell_var_init();

    register_cmd("help", "General", "help [cmd]", cmd_help);
    register_cmd("echo", "General", "echo ARG...", cmd_echo);
    register_cmd("add", "General", "add time", cmd_add);
    register_cmd("remove", "General", "remove time", cmd_remove);
    register_command(&(struct cmd_reg){
        .name = "ls",
        .category = "Filesystem",
        .synopsis = "ls [dir] - list directory contents",
        .fn = cmd_ls,
        .args = fs_ls_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "cd",
        .category = "Filesystem",
        .synopsis = "cd <dir> - change current directory",
        .fn = cmd_cd,
        .args = fs_dir_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "mkdir",
        .category = "Filesystem",
        .synopsis = "mkdir <dir> - create directory",
        .fn = cmd_mkdir,
        .args = fs_dir_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "mv",
        .category = "Filesystem",
        .synopsis = "mv <src> <dst> - move/rename file or directory",
        .fn = cmd_mv,
        .args = fs_mv_args,
        .nargs = 2,
    });
    register_command(&(struct cmd_reg){
        .name = "cat",
        .category = "Filesystem",
        .synopsis = "cat <path> - output file contents",
        .fn = cmd_cat,
        .args = fs_path_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "exists",
        .category = "Filesystem",
        .synopsis = "exists <path> - test if path exists (exit code)",
        .fn = cmd_exists,
        .args = fs_path_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "size",
        .category = "Filesystem",
        .synopsis = "size <path> - return file size in bytes",
        .fn = cmd_size,
        .args = fs_path_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "rm",
        .category = "Filesystem",
        .synopsis = "rm <path> - remove file",
        .fn = cmd_rm,
        .args = fs_path_args,
        .nargs = 1,
    });
    extern void cmd_cp_register(void);
    cmd_cp_register();
    extern void cmd_eval_register(void);
    cmd_eval_register();

    // Install the top-level object-root methods (cp, peeler, rom_probe,
    // …) so JS callers (`gsEval`) can use them before any machine boots.
    extern void gs_classes_install_root(void);
    gs_classes_install_root();

    // Latch the worker pthread for the thread-affinity guard. From now
    // on (under MODE=debug/sanitize) any call into shell_dispatch() or
    // gs_eval() from a different thread aborts with GS_ASSERTF.
    gs_thread_record_worker();

    shell_initialized = 1;
    return 0;
}
