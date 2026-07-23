// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// log.c
// Implements the logging framework: category registry, runtime levels, sinks, and shell command.

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h" // debug_trace_capture_log()
#include "log.h"
#include "scheduler.h" // cpu_instr_count()
#include "shell.h"

// Holds a single logging category node
struct log_category {
    char *name; // Category name (owned)
    int level; // Current level threshold; 0 = off
    int to_stdout; // Emit to stdout
    int timestamp; // Include cpu_instr_count() prefix
    int show_pc; // Include PC register in output
    char *file_path; // Optional file sink path (owned)
    FILE *file_fp; // Opened file handle (append mode)
    struct log_category *next; // Next in registry list
};

// Global registry head (singly-linked list)
static struct log_category *s_registry_head = NULL;

// Optional global sink (in addition to per-category stdout/file). Not set by default.
static log_sink_fn s_sink_fn = NULL;
static void *s_sink_user = NULL;

#define LOG_MAX_INDENT_SPACES 64
static int s_indent_spaces = 0;

// Clamps the requested indent width into the supported range.
static int clamp_indent(int spaces) {
    if (spaces < 0)
        return 0;
    if (spaces > LOG_MAX_INDENT_SPACES)
        return LOG_MAX_INDENT_SPACES;
    return spaces;
}

// Finds a category by name; returns pointer or NULL
static struct log_category *find_category(const char *name) {
    for (struct log_category *c = s_registry_head; c; c = c->next) {
        if (strcmp(c->name, name) == 0)
            return c;
    }
    return NULL;
}

// Creates a new category node; NULL on failure
static struct log_category *create_category(const char *name) {
    struct log_category *c = (struct log_category *)malloc(sizeof(*c));
    if (!c)
        return NULL;
    c->name = strdup(name ? name : "");
    if (!c->name) {
        free(c);
        return NULL;
    }
    c->level = 0; // default level is 0 (OFF)
    c->to_stdout = 1; // default to stdout when enabled
    c->timestamp = 0; // default no timestamp
    c->show_pc = 0; // default no PC register
    c->file_path = NULL;
    c->file_fp = NULL;
    c->next = NULL;
    return c;
}

// Close file sink for a category (if open)
static void close_category_file(struct log_category *c) {
    if (!c)
        return;
    if (c->file_fp) {
        fflush(c->file_fp);
        fclose(c->file_fp);
        c->file_fp = NULL;
    }
}

// Set file sink path ("off" or NULL disables). Returns 0 on success, -1 on error (keeps previous).
static int set_category_file(struct log_category *c, const char *path) {
    if (!c)
        return -1;
    if (!path || strcmp(path, "off") == 0 || *path == '\0') {
        // disable file sink
        close_category_file(c);
        free(c->file_path);
        c->file_path = NULL;
        return 0;
    }

    // Try opening new file in append mode first, to ensure it works
    FILE *fp = fopen(path, "a");
    if (!fp) {
        fprintf(stderr, "log: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }

    // Swap in new handle
    close_category_file(c);
    free(c->file_path);
    c->file_path = strdup(path);
    if (!c->file_path) {
        fclose(fp);
        return -1;
    }
    c->file_fp = fp;
    return 0;
}

// Prints one category's level/sink/timestamp/PC configuration.
static void print_category_config(const struct log_category *c) {
    if (!c)
        return;
    printf("%s level=%d stdout=%s file=%s ts=%s pc=%s\n", c->name, c->level, c->to_stdout ? "on" : "off",
           c->file_path ? c->file_path : "off", c->timestamp ? "on" : "off", c->show_pc ? "on" : "off");
}

static int parse_onoff(const char *v, int *out) {
    if (!v || !out)
        return -1;
    if (strcasecmp(v, "on") == 0 || strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0) {
        *out = 1;
        return 0;
    }
    if (strcasecmp(v, "off") == 0 || strcasecmp(v, "false") == 0 || strcmp(v, "0") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

// Apply one whitespace-delimited config token to category `c`: a bare
// non-negative integer sets the level; `key=val` sets stdout / file /
// ts / pc / level. `tok` is a NUL-terminated copy the caller owns.
static void apply_log_token(struct log_category *c, char *tok) {
    char *eq = strchr(tok, '=');
    if (!eq) {
        char *end = NULL;
        long lvl = strtol(tok, &end, 10);
        if (end && *end == '\0' && lvl >= 0)
            c->level = (int)lvl;
        else
            printf("log: unrecognized option '%s'\n", tok);
        return;
    }
    *eq = '\0';
    const char *key = tok;
    const char *val = eq + 1;
    if (strcasecmp(key, "level") == 0) {
        char *end = NULL;
        long lvl = strtol(val, &end, 10);
        if (!(end && *end == '\0') || lvl < 0)
            puts("log: invalid level");
        else
            c->level = (int)lvl;
    } else if (strcasecmp(key, "stdout") == 0) {
        int b;
        if (parse_onoff(val, &b) == 0)
            c->to_stdout = b;
        else
            puts("log: stdout expects on/off");
    } else if (strcasecmp(key, "file") == 0) {
        (void)set_category_file(c, val); // set_category_file prints on failure
    } else if (strcasecmp(key, "ts") == 0 || strcasecmp(key, "timestamp") == 0) {
        int b;
        if (parse_onoff(val, &b) == 0)
            c->timestamp = b;
        else
            puts("log: ts expects on/off");
    } else if (strcasecmp(key, "pc") == 0) {
        int b;
        if (parse_onoff(val, &b) == 0)
            c->show_pc = b;
        else
            puts("log: pc expects on/off");
    } else {
        printf("log: unknown key '%s'", key);
    }
}

// `debug.log(category, spec)` core. `category` names the log category
// (created on demand); `spec` is NULL/empty to just print the current
// config, or a whitespace-delimited option string
// (`"5"`, `"level=5 file=tmp/x.log stdout=off ts=on"`). Returns 0.
int log_configure(const char *category, const char *spec) {
    if (!category || !*category) {
        for (struct log_category *c = s_registry_head; c; c = c->next)
            print_category_config(c);
        return 0;
    }
    struct log_category *c = find_category(category);
    if (!spec || !*spec) {
        if (!c) {
            printf("unknown category \"%s\"\n", category);
            return 0;
        }
        print_category_config(c);
        return 0;
    }
    // Setting options: create the category on demand for pre-config.
    if (!c) {
        c = create_category(category);
        if (!c) {
            puts("log: out of memory");
            return -1;
        }
        c->next = s_registry_head;
        s_registry_head = c;
    }
    // Split `spec` on whitespace into tokens and apply each.
    char *copy = strdup(spec);
    if (!copy) {
        puts("log: out of memory");
        return -1;
    }
    char *save = NULL;
    for (char *tok = strtok_r(copy, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save))
        apply_log_token(c, tok);
    free(copy);
    print_category_config(c);
    return 0;
}

// Public API ----------------------------------------------------------------

// Initializes the logging system. Phase 5c: legacy `log` shell command
// registration retired; the typed `log_set` root method calls cmd_log
// directly.
void log_init(void) {
    if (!s_sink_fn)
        s_sink_fn = NULL;
}

// Registers a category by name (idempotent). Level defaults to 0 on first create.
log_category_t *log_register_category(const char *name) {
    if (!name || !*name)
        return NULL;
    struct log_category *c = find_category(name);
    if (c)
        return c; // return existing; level unchanged

    c = create_category(name);
    if (!c)
        return NULL;

    // Insert at head
    c->next = s_registry_head;
    s_registry_head = c;
    return c;
}

// Returns the category for 'name' or NULL if not found.
log_category_t *log_get_category(const char *name) {
    if (!name)
        return NULL;
    return find_category(name);
}

// Returns a category's name, or NULL if 'cat' is NULL.
const char *log_category_name(const log_category_t *cat) {
    return cat ? cat->name : NULL;
}

// Returns the current level, or 0 if 'cat' is NULL.
int log_get_level(const log_category_t *cat) {
    return cat ? cat->level : 0;
}

// Sets 'cat' level; returns previous level, or -1 on error.
int log_set_level(log_category_t *cat, int level) {
    if (!cat || level < 0)
        return -1;
    int prev = cat->level;
    cat->level = level;
    return prev;
}

// Visit every registered category in registration order.
void log_foreach_category(void (*fn)(const log_category_t *cat, void *ud), void *ud) {
    if (!fn)
        return;
    for (const struct log_category *c = s_registry_head; c; c = c->next)
        fn(c, ud);
}

// Sets the output sink (or defaults to stdout when fn == NULL).
void log_set_sink(log_sink_fn fn, void *user) {
    s_sink_fn = fn ? fn : NULL; // when NULL, only per-category sinks are used
    s_sink_user = user;
}

// Sets the indentation width (in spaces) applied before each message body.
void log_indent_set(int spaces) {
    s_indent_spaces = clamp_indent(spaces);
}

// Returns the current indentation width in spaces.
int log_indent_get(void) {
    return s_indent_spaces;
}

// Adjusts the indentation width by delta then clamps.
void log_indent_adjust(int delta) {
    if (delta == 0)
        return;
    log_indent_set(s_indent_spaces + delta);
}

// Emits a log line using a va_list. Final form: "[name] level message\n"
void log_vemit(const log_category_t *cat, int level, const char *fmt, va_list ap) {
    if (!cat)
        return; // Treat missing category as disabled

    const struct log_category *c = (const struct log_category *)cat;

    // Format the message body first to avoid computing prefix twice.
    char body[512];
    int n = vsnprintf(body, sizeof(body), fmt ? fmt : "", ap);
    if (n < 0)
        return; // formatting error; ignore
    body[sizeof(body) - 1] = '\0';

    // Compose the final line with optional timestamp and/or PC
    char line[768];
    const char *name = c->name ? c->name : "";
    const int indent_spaces = s_indent_spaces;
    char indent_buf[LOG_MAX_INDENT_SPACES + 1];
    if (indent_spaces > 0) {
        memset(indent_buf, ' ', (size_t)indent_spaces);
        indent_buf[indent_spaces] = '\0';
    }

    // Determine PC value if needed
    uint32_t pc_value = 0;
    if (c->show_pc) {
        cpu_t *cpu = system_cpu();
        if (cpu) {
            extern uint32_t cpu_get_pc(cpu_t *restrict cpu);
            pc_value = cpu_get_pc(cpu);
        }
    }

    // Format line with timestamp and/or PC as needed
    if (c->timestamp && c->show_pc) {
        unsigned long long t = (unsigned long long)cpu_instr_count();
        if (indent_spaces > 0)
            snprintf(line, sizeof(line), "[%s] %d @%llu PC=%08x %s%s\n", name, level, t, (unsigned)pc_value, indent_buf,
                     body);
        else
            snprintf(line, sizeof(line), "[%s] %d @%llu PC=%08x %s\n", name, level, t, (unsigned)pc_value, body);
    } else if (c->timestamp) {
        unsigned long long t = (unsigned long long)cpu_instr_count();
        if (indent_spaces > 0)
            snprintf(line, sizeof(line), "[%s] %d @%llu %s%s\n", name, level, t, indent_buf, body);
        else
            snprintf(line, sizeof(line), "[%s] %d @%llu %s\n", name, level, t, body);
    } else if (c->show_pc) {
        if (indent_spaces > 0)
            snprintf(line, sizeof(line), "[%s] %d PC=%08x %s%s\n", name, level, (unsigned)pc_value, indent_buf, body);
        else
            snprintf(line, sizeof(line), "[%s] %d PC=%08x %s\n", name, level, (unsigned)pc_value, body);
    } else {
        if (indent_spaces > 0)
            snprintf(line, sizeof(line), "[%s] %d %s%s\n", name, level, indent_buf, body);
        else
            snprintf(line, sizeof(line), "[%s] %d %s\n", name, level, body);
    }

    // Emit to configured sinks
    if (c->to_stdout) {
        fputs(line, stdout);
        fflush(stdout);
    }
    if (c->file_fp) {
        fputs(line, c->file_fp);
        fflush(c->file_fp);
    }

    // Also pass to optional global sink if one was installed by the host
    if (s_sink_fn)
        s_sink_fn(line, s_sink_user);

    // Capture to trace buffer if tracing is active
    if (debug_trace_is_active()) {
        debug_trace_capture_log(line);
    }
}

// Convenience wrapper for variadic emission.
void log_emit(const log_category_t *cat, int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vemit(cat, level, fmt, ap);
    va_end(ap);
}
