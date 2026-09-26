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

#include "log_categories.h"

#include "ppc.h" // PowerPC pc / r24 for the PC decoration
#include "scheduler.h" // cpu_instr_count()
#include "shell.h"
#include "system.h" // system_config() / system_cpu()
#include "system_config.h" // config_t::ppc
#include "value.h"

static bool name_in_manifest(const char *name);

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

// === Typed configuration ====================================================
//
// These replace log_configure(category, "level=5 stdout=off file=..."), which
// was a flag grammar inside a string parsed with strtok_r -- the exact shape
// docs/core/shell/object-model.md ("Library conventions") says named
// arguments exist to retire.
// The framework could not validate it (the slot was declared V_NONE, so it
// was told nothing to validate), completion could not offer the keys or their
// values, and it carried its own boolean vocabulary and its own error wording.
//
// Each setter takes the category by name so the caller does not have to hold
// a handle, and validates against the manifest by going through
// log_register_category.

int log_set_category_level(const char *category, int level) {
    if (level < 0)
        return -1;
    log_category_t *c = log_register_category(category);
    if (!c)
        return -1;
    log_set_level(c, level);
    return 0;
}

int log_set_category_stdout(const char *category, bool on) {
    struct log_category *c = (struct log_category *)log_register_category(category);
    if (!c)
        return -1;
    c->to_stdout = on;
    return 0;
}

int log_set_category_timestamp(const char *category, bool on) {
    struct log_category *c = (struct log_category *)log_register_category(category);
    if (!c)
        return -1;
    c->timestamp = on;
    return 0;
}

int log_set_category_show_pc(const char *category, bool on) {
    struct log_category *c = (struct log_category *)log_register_category(category);
    if (!c)
        return -1;
    c->show_pc = on;
    return 0;
}

// `path` NULL or "off" closes any open file for this category.
int log_set_category_file(const char *category, const char *path) {
    struct log_category *c = (struct log_category *)log_register_category(category);
    if (!c)
        return -1;
    return set_category_file(c, path ? path : "off");
}

// Print one category's current settings, as `debug.log <cat>` does.
void log_print_category(const char *category) {
    struct log_category *c = (struct log_category *)log_get_category(category);
    if (!c) {
        printf("unknown category \"%s\" (see debug.log_levels() for the full list)\n", category);
        return;
    }
    print_category_config(c);
}

// Public API ----------------------------------------------------------------

// Initializes the logging system. The legacy `log` shell command
// registration is retired; the typed `log_set` root method calls cmd_log
// directly.
void log_init(void) {
    if (!s_sink_fn)
        s_sink_fn = NULL;
}

// === The manifest =========================================================

static bool name_in_manifest(const char *name) {
#define X(n, lvl, desc)                                                                                                \
    if (strcmp(name, (n)) == 0)                                                                                        \
        return true;
    GS_LOG_CATEGORIES(X)
#undef X
    (void)name;
    return false;
}

const char *log_category_description(const char *name) {
    if (!name)
        return NULL;
#define X(n, lvl, desc)                                                                                                \
    if (strcmp(name, (n)) == 0)                                                                                        \
        return (desc);
    GS_LOG_CATEGORIES(X)
#undef X
    return NULL;
}

// Create every category the manifest declares, so `debug.log` with no
// arguments lists the real, complete set rather than only what has been hit
// or configured so far.
void log_register_manifest(void) {
#define X(n, lvl, desc)                                                                                                \
    do {                                                                                                               \
        log_category_t *c = log_register_category(n);                                                                  \
        if (c && (lvl) != 0)                                                                                           \
            log_set_level(c, (lvl));                                                                                   \
    } while (0);
    GS_LOG_CATEGORIES(X)
#undef X
}

// Registers a category by name (idempotent). Level defaults to 0 on first create.
log_category_t *log_register_category(const char *name) {
    if (!name || !*name)
        return NULL;
    // A category that is not in the manifest is a typo, in code or in a
    // `debug.log` argument.  It used to be created on the spot, which is how
    // `debug.log cpuu 10` reported success and produced nothing.
    GS_ASSERTF(name_in_manifest(name), "log category '%s' is not in GS_LOG_CATEGORIES", name);
    if (!name_in_manifest(name))
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

    // Determine the PC decoration if needed.  On a PowerPC machine the
    // main CPU is the 601/604 and the interesting "PC" for driver-level
    // logs is usually the emulated 68k one, which the ROM's emulator keeps
    // in r24 while 68k code runs — show both.
    char pcstr[40] = "";
    if (c->show_pc) {
        config_t *cfg = system_config();
        if (cfg && cfg->ppc) {
            snprintf(pcstr, sizeof(pcstr), "PC=%08x r24=%08x", (unsigned)ppc_get_pc(cfg->ppc),
                     (unsigned)ppc_get_gpr(cfg->ppc, 24));
        } else {
            cpu_t *cpu = system_cpu();
            uint32_t pc_value = 0;
            if (cpu) {
                extern uint32_t cpu_get_pc(cpu_t *restrict cpu);
                pc_value = cpu_get_pc(cpu);
            }
            snprintf(pcstr, sizeof(pcstr), "PC=%08x", (unsigned)pc_value);
        }
    }

    // Format line with timestamp and/or PC as needed
    if (c->timestamp && c->show_pc) {
        unsigned long long t = (unsigned long long)cpu_instr_count();
        if (indent_spaces > 0)
            snprintf(line, sizeof(line), "[%s] %d @%llu %s %s%s\n", name, level, t, pcstr, indent_buf, body);
        else
            snprintf(line, sizeof(line), "[%s] %d @%llu %s %s\n", name, level, t, pcstr, body);
    } else if (c->timestamp) {
        unsigned long long t = (unsigned long long)cpu_instr_count();
        if (indent_spaces > 0)
            snprintf(line, sizeof(line), "[%s] %d @%llu %s%s\n", name, level, t, indent_buf, body);
        else
            snprintf(line, sizeof(line), "[%s] %d @%llu %s\n", name, level, t, body);
    } else if (c->show_pc) {
        if (indent_spaces > 0)
            snprintf(line, sizeof(line), "[%s] %d %s %s%s\n", name, level, pcstr, indent_buf, body);
        else
            snprintf(line, sizeof(line), "[%s] %d %s %s\n", name, level, pcstr, body);
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
