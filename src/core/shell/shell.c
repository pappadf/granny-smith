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
static char current_dir[256] = "/";

static uint64_t cmd_ls(int argc, char *argv[]) {
    const char *path = current_dir;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            if (argv[i][0] == '-' && argv[i][1] != '\0')
                continue;
            path = argv[i];
            break;
        }
    }
    DIR *dir = opendir(path);
    if (!dir) {
        printf("ls: cannot open directory '%s': %s\n", path, strerror(errno));
        return 0;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
        printf("%s\n", entry->d_name);
    closedir(dir);
    return 0;
}

static void resolve_path(const char *input, char *out, size_t outlen) {
    char buf[256];
    if (input[0] == '/')
        snprintf(buf, sizeof(buf), "%s", input);
    else
        snprintf(buf, sizeof(buf), "%s/%s", current_dir, input);

    const char *components[64];
    int depth = 0;
    char *saveptr = NULL;
    char *token = strtok_r(buf, "/", &saveptr);
    while (token) {
        if (strcmp(token, ".") == 0) { /* skip */
        } else if (strcmp(token, "..") == 0) {
            if (depth > 0)
                depth--;
        } else
            components[depth++] = token;
        token = strtok_r(NULL, "/", &saveptr);
    }
    if (depth == 0) {
        snprintf(out, outlen, "/");
    } else {
        out[0] = '\0';
        for (int i = 0; i < depth; i++) {
            size_t len = strlen(out);
            snprintf(out + len, outlen - len, "/%s", components[i]);
        }
    }
}

static uint64_t cmd_cd(int argc, char *argv[]) {
    if (argc != 2) {
        printf("usage: cd <directory>\n");
        return 0;
    }
    char resolved[256];
    resolve_path(argv[1], resolved, sizeof(resolved));
    if (chdir(resolved) == 0) {
        snprintf(current_dir, sizeof(current_dir), "%s", resolved);
        printf("Changed directory to %s\n", current_dir);
    } else {
        printf("cd: cannot change to '%s': %s\n", argv[1], strerror(errno));
    }
    return 0;
}

static uint64_t cmd_mkdir(int argc, char *argv[]) {
    if (argc != 2) {
        printf("usage: mkdir <directory>\n");
        return 0;
    }
    if (mkdir(argv[1], 0777) == 0)
        printf("Directory '%s' created\n", argv[1]);
    else
        printf("mkdir: cannot create directory '%s': %s\n", argv[1], strerror(errno));
    return 0;
}

static uint64_t cmd_mv(int argc, char *argv[]) {
    if (argc != 3) {
        printf("usage: mv <source> <destination>\n");
        return 0;
    }
    if (rename(argv[1], argv[2]) == 0)
        printf("Moved '%s' to '%s'\n", argv[1], argv[2]);
    else
        printf("mv: cannot move '%s' to '%s': %s\n", argv[1], argv[2], strerror(errno));
    return 0;
}

static uint64_t cmd_cat(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: cat <path>\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        printf("cat: cannot open '%s': %s\n", argv[1], strerror(errno));
        return 1;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        fwrite(buf, 1, n, stdout);
    fclose(f);
    return 0;
}

static uint64_t cmd_exists(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: exists <path>\n");
        return 1;
    }
    struct stat st;
    return (stat(argv[1], &st) == 0) ? 0 : 1;
}

static uint64_t cmd_size(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: size <path>\n");
        return 0;
    }
    struct stat st;
    if (stat(argv[1], &st) != 0) {
        printf("size: cannot stat '%s': %s\n", argv[1], strerror(errno));
        return 0;
    }
    return (uint64_t)st.st_size;
}

static uint64_t cmd_rm(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: rm <path>\n");
        return 1;
    }
    if (unlink(argv[1]) != 0) {
        printf("rm: cannot remove '%s': %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}

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

    char *argv[MAXTOK];
    int argc = tokenize(line, argv, MAXTOK);
    if (argc <= 0)
        return;

    struct cmd_reg_node *c = find_cmd(argv[0]);
    if (c) {
        execute_cmd(c, argc, argv, mode, res);
        return;
    }

    cmd_err(res, "unknown command: %s", argv[0]);
}

// Dispatch interactively and return integer result
uint64_t shell_dispatch(char *line) {
    if (!shell_initialized)
        return -1;

    char *argv[MAXTOK];
    int argc = tokenize(line, argv, MAXTOK);
    if (argc < 0) {
        fputs("too many arguments\n", stderr);
        return 0;
    }
    if (argc == 0)
        return 0;

    struct cmd_reg_node *c = find_cmd(argv[0]);
    if (!c) {
        suggest_command(argv[0]);
        return 0;
    }

    struct cmd_result res;
    memset(&res, 0, sizeof(res));
    execute_cmd(c, argc, argv, INVOKE_INTERACTIVE, &res);

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

    register_cmd("help", "General", "help [cmd]", cmd_help);
    register_cmd("echo", "General", "echo ARG...", cmd_echo);
    register_cmd("add", "General", "add time", cmd_add);
    register_cmd("remove", "General", "remove time", cmd_remove);
    register_cmd("ls", "Filesystem", "ls [dir] - list directory contents", cmd_ls);
    register_cmd("cd", "Filesystem", "cd <dir> - change current directory", cmd_cd);
    register_cmd("mkdir", "Filesystem", "mkdir <dir> - create directory", cmd_mkdir);
    register_cmd("mv", "Filesystem", "mv <src> <dst> - move/rename file or directory", cmd_mv);
    register_cmd("cat", "Filesystem", "cat <path> - output file contents", cmd_cat);
    register_cmd("exists", "Filesystem", "exists <path> - test if path exists (exit code)", cmd_exists);
    register_cmd("size", "Filesystem", "size <path> - return file size in bytes", cmd_size);
    register_cmd("rm", "Filesystem", "rm <path> - remove file", cmd_rm);

    shell_initialized = 1;
    return 0;
}
