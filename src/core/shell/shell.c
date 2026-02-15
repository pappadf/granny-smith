// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell.c
// Interactive command shell for emulator debugging and control.

#include "shell.h"

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

/* --- command interface --------------------------------------------------- */

struct cmd_node {
    char *name;
    char *category;
    char *synopsis;
    cmd_fn fn;
    struct cmd_node *next;
};

static struct cmd_node *cmd_head = NULL;

/* --- command registry ----------------------------------------------------- */
static struct cmd_node *find_cmd(const char *name, struct cmd_node **prev_out) {
    struct cmd_node *prev = NULL, *cur = cmd_head;
    while (cur) {
        if (strcmp(cur->name, name) == 0) {
            if (prev_out)
                *prev_out = prev;
            return cur;
        }
        prev = cur;
        cur = cur->next;
    }
    return NULL;
}

int register_cmd(const char *name, const char *category, const char *synopsis, cmd_fn fn) {
    if (find_cmd(name, NULL))
        return -1; /* already exists */
    struct cmd_node *n = malloc(sizeof *n);
    if (!n)
        return -1;
    n->name = strdup(name);
    n->category = strdup(category);
    n->synopsis = strdup(synopsis);
    n->fn = fn;
    n->next = cmd_head; /* insert at head */
    cmd_head = n;
    return 0;
}

int unregister_cmd(const char *name) {
    struct cmd_node *prev, *cur = find_cmd(name, &prev);
    if (!cur)
        return -1;
    if (prev)
        prev->next = cur->next;
    else
        cmd_head = cur->next;
    free(cur->name);
    free(cur->category);
    free(cur->synopsis);
    free(cur);
    return 0;
}

/* --- tokenizer (unchanged) ------------------------------------------------ */
#define MAXTOK 32
static int tokenize(char *line, char *argv[], int max) {
    // Enhanced tokenizer with support for: \-escapes, ASCII '"' and '\'' quotes,
    // and UTF-8 curly quotes U+201C/U+201D as quote delimiters.
    // Curly quotes are removed from the output similar to ASCII quotes.

    int argc = 0;
    int esc = 0;
    enum { Q_NONE = 0, Q_DQUOTE, Q_SQUOTE, Q_CURLY } qstate = Q_NONE;

    char *p = line;
    while (*p) {
        // skip leading whitespace when not in a token
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (argc == max)
            return -1;

        argv[argc++] = p; // token starts here (we will compact in place)
        char *dst = p;

        for (;;) {
            unsigned char c = (unsigned char)*p;
            if (c == '\0') {
                *dst = '\0';
                break;
            }

            if (esc) {
                // Copy char literally and clear escape
                *dst++ = *p++;
                esc = 0;
                continue;
            }

            // Detect UTF-8 curly quotes (E2 80 9C/9D)
            if ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x80 &&
                ((unsigned char)p[2] == 0x9C || (unsigned char)p[2] == 0x9D)) {
                if (qstate == Q_NONE) {
                    qstate = Q_CURLY; // open
                } else if (qstate == Q_CURLY) {
                    qstate = Q_NONE; // close
                } else {
                    // inside other quote type: treat as data
                    *dst++ = *p++;
                    continue;
                }
                p += 3; // consume the curly quote bytes (not copied)
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
                else {
                    *dst++ = *p;
                }
                p++;
                continue;
            }

            if (*p == '\'') {
                if (qstate == Q_NONE)
                    qstate = Q_SQUOTE;
                else if (qstate == Q_SQUOTE)
                    qstate = Q_NONE;
                else {
                    *dst++ = *p;
                }
                p++;
                continue;
            }

            if (qstate == Q_NONE && isspace((unsigned char)*p)) {
                // end of token on unquoted whitespace
                *dst = '\0';
                // advance p past the space so outer loop can skip further spaces
                p++;
                break;
            }

            // normal char
            *dst++ = *p++;
        }
    }
    return argc;
}

/* --- built-in commands ---------------------------------------------------- */
static uint64_t cmd_help(int argc, char *argv[]) {
    if (argc == 1) {
        const char *cat = NULL;
        for (struct cmd_node *c = cmd_head; c; c = c->next) {
            if (cat == NULL || strcmp(cat, c->category) != 0) {
                cat = c->category;
                printf("\n%s:\n", cat);
            }
            printf("  %-10s %s\n", c->name, c->synopsis);
        }
    } else {
        for (int i = 1; i < argc; ++i) {
            struct cmd_node *c = find_cmd(argv[i], NULL);
            if (c)
                printf("%s — %s\n", c->name, c->synopsis);
            else
                printf("unknown command \"%s\"\n", argv[i]);
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

/* Control commands to add/remove the demo command */
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
static char current_dir[256] = "/"; // Track current working directory

static uint64_t cmd_ls(int argc, char *argv[]) {
    /* Always output one entry per line (ls -1 style). Ignore option-looking args. */
    const char *path = current_dir;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            if (argv[i][0] == '-' && argv[i][1] != '\0')
                continue; /* skip pseudo options */
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
    while ((entry = readdir(dir)) != NULL) {
        printf("%s\n", entry->d_name);
    }
    closedir(dir);
    return 0;
}

static uint64_t cmd_cd(int argc, char *argv[]) {
    if (argc != 2) {
        printf("usage: cd <directory>\n");
        return 0;
    }
    if (chdir(argv[1]) == 0) {
        if (getcwd(current_dir, sizeof(current_dir)))
            printf("Changed directory to %s\n", current_dir);
        else
            printf("cd: error getting current directory\n");
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
    if (mkdir(argv[1], 0777) == 0) {
        printf("Directory '%s' created\n", argv[1]);
    } else {
        printf("mkdir: cannot create directory '%s': %s\n", argv[1], strerror(errno));
    }
    return 0;
}

static uint64_t cmd_mv(int argc, char *argv[]) {
    if (argc != 3) {
        printf("usage: mv <source> <destination>\n");
        return 0;
    }
    if (rename(argv[1], argv[2]) == 0) {
        printf("Moved '%s' to '%s'\n", argv[1], argv[2]);
    } else {
        printf("mv: cannot move '%s' to '%s': %s\n", argv[1], argv[2], strerror(errno));
    }
    return 0;
}

// Output file contents to stdout
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

// Test if path exists (return 0 = exists, 1 = not found)
static uint64_t cmd_exists(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: exists <path>\n");
        return 1;
    }
    struct stat st;
    return (stat(argv[1], &st) == 0) ? 0 : 1;
}

// Return file size in bytes (returns 0 on error)
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

// Remove a file
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

/* --- dispatcher ----------------------------------------------------------- */
uint64_t shell_dispatch(char *line) {
    if (!shell_initialized) {
        return -1;
    }

    char *argv[MAXTOK];
    int argc = tokenize(line, argv, MAXTOK);
    if (argc < 0) {
        fputs("too many arguments\n", stderr);
        return 0;
    }
    if (argc == 0)
        return 0;
    struct cmd_node *c = find_cmd(argv[0], NULL);
    if (!c) {
        fprintf(stderr, "unknown command \"%s\"\n", argv[0]);
        return 0;
    }

    return c->fn(argc, argv);
}

// Handle command input from platform layer
uint64_t handle_command(const char *input_line) {
    if (!input_line || !shell_initialized)
        return -1;

    size_t len = strlen(input_line);
    while (len > 0 && (input_line[len - 1] == '\n' || input_line[len - 1] == '\r')) {
        len--;
    }

    char *mutable_line = (char *)malloc(len + 1);
    if (!mutable_line)
        return -1;

    memcpy(mutable_line, input_line, len);
    mutable_line[len] = '\0';

    uint64_t result = shell_dispatch(mutable_line);

    free(mutable_line);
    return result;
}

/* --- main ----------------------------------------------------------------- */
int shell_init(void) {
    if (shell_initialized)
        return 0;
    // Initialize logging; registers the `log` shell command
    log_init();

    // Initialize peeler; registers the `peeler` shell command
    peeler_shell_init();

    /* register built-ins */
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
