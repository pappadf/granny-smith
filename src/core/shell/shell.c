// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell.c
// Interactive command shell for emulator debugging and control.

#include "shell.h"

#include "cmd_complete.h"
#include "cmd_io.h"
#include "cmd_json.h"
#include "cmd_parse.h"
#include "cmd_types.h"
#include "log.h"
#include "peeler_shell.h"
#include "shell_var.h"
#include "vfs.h"

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

static int shell_initialized = 0;

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
static int tokenize(char *line, char *argv[], int max) {
    // Tokenizer with support for: \-escapes, ASCII and UTF-8 curly quotes.
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

    struct cmd_reg_node *c = find_cmd(argv[0]);
    if (c) {
        execute_cmd(c, argc, argv, mode, res);
        free(expanded);
        return;
    }

    // Unknown command: print suggestion, but return OK (exit code 0)
    // to match the convention that unknown commands are not fatal errors.
    suggest_command(argv[0]);
    res->type = RES_OK;
    free(expanded);
}

// Dispatch interactively and return integer result
uint64_t shell_dispatch(char *line) {
    if (!shell_initialized)
        return -1;

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

    struct cmd_reg_node *c = find_cmd(argv[0]);
    if (!c) {
        suggest_command(argv[0]);
        free(expanded);
        return 0;
    }

    struct cmd_result res;
    memset(&res, 0, sizeof(res));
    execute_cmd(c, argc, argv, INVOKE_INTERACTIVE, &res);
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

// Handle command input from platform layer
uint64_t handle_command(const char *input_line) {
    if (!input_line || !shell_initialized)
        return -1;

    size_t len = strlen(input_line);
    while (len > 0 && (input_line[len - 1] == '\n' || input_line[len - 1] == '\r'))
        len--;

    char *mutable_line = (char *)malloc(len + 1);
    if (!mutable_line)
        return -1;

    memcpy(mutable_line, input_line, len);
    mutable_line[len] = '\0';

    uint64_t result = shell_dispatch(mutable_line);

    free(mutable_line);
    return result;
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

    shell_initialized = 1;
    return 0;
}
