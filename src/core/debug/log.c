// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// log.c
// Implements the logging framework: category registry, runtime levels, sinks and line formatting.

// A leaf module: it knows nothing of the CPU, the scheduler or the debugger.
// What a line is decorated with (instruction count, PC) and who else sees
// each line (the debug trace) comes in through log_set_context_hooks
// (installed by log_context.c).

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

#include "gs_assert.h"
#include "log_categories.h"

static bool name_in_manifest(const char *name);

// Holds a single logging category node
struct log_category {
    char *name; // Category name (owned)
    int level; // Current level threshold; 0 = off
    int to_stdout; // Emit to stdout
    int timestamp; // Include the instruction-count prefix
    int show_pc; // Include PC register in output
    char *file_path; // Optional file sink path (owned)
    FILE *file_fp; // Opened file handle (append mode)
    struct log_category *next; // Next in registry list
};

// Global registry head (singly-linked list, newest first), for enumeration
static struct log_category *s_registry_head = NULL;

// Number of categories the manifest declares (each X() adds one)
#define LOG_COUNT_ONE(n, lvl, desc) +1
enum { LOG_MANIFEST_COUNT = 0 GS_LOG_CATEGORIES(LOG_COUNT_ONE) };
#undef LOG_COUNT_ONE

// Name -> category index: open addressing with linear probing, sized to at
// least twice the manifest so probes stay short and a free slot always exists
#define LOG_TABLE_SLOTS 256
_Static_assert(LOG_MANIFEST_COUNT * 2 <= LOG_TABLE_SLOTS, "grow LOG_TABLE_SLOTS with the category manifest");
static struct log_category *s_table[LOG_TABLE_SLOTS];

// Context hooks (decorations, line observer); NULL until installed
static const log_context_hooks_t *s_hooks = NULL;

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

// FNV-1a hash of a category name, for the lookup table
static uint32_t name_hash(const char *name) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = (h ^ *p) * 16777619u;
    return h;
}

// Returns the table slot holding `name`, or the empty slot where it would go
static struct log_category **registry_slot(const char *name) {
    uint32_t i = name_hash(name) & (LOG_TABLE_SLOTS - 1);
    // The table is never full (static assert above), so the probe ends
    while (s_table[i] && strcmp(s_table[i]->name, name) != 0)
        i = (i + 1) & (LOG_TABLE_SLOTS - 1);
    return &s_table[i];
}

// Registry lookup: the mutable node for `name`, or NULL when not registered.
// (log_get_category is the public, NULL-tolerant face of this.)
static struct log_category *registry_lookup(const char *name) {
    return *registry_slot(name);
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
    if (!c) {
        errno = EINVAL; // no category: not a file problem, but say why
        return -1;
    }
    if (!path || strcmp(path, "off") == 0 || *path == '\0') {
        // disable file sink
        close_category_file(c);
        free(c->file_path);
        c->file_path = NULL;
        return 0;
    }

    // Try opening new file in append mode first, to ensure it works; on
    // failure errno is left as fopen set it, for the caller to report
    FILE *fp = fopen(path, "a");
    if (!fp)
        return -1;

    // Swap in new handle
    close_category_file(c);
    free(c->file_path);
    c->file_path = strdup(path);
    if (!c->file_path) {
        fclose(fp);
        errno = ENOMEM; // fclose may have overwritten strdup's errno
        return -1;
    }
    c->file_fp = fp;
    return 0;
}

// === Typed configuration ====================================================
//
// These replace log_configure(category, "level=5 stdout=off file=..."), which
// was a flag grammar inside a string parsed with strtok_r -- the exact shape
// docs/internals/core/object/object-model.md ("Library conventions") says named
// arguments exist to retire.
// The framework could not validate it (the slot was declared VK_NONE, so it
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
    if (!c) {
        // Not a file error either: an unknown name, or no memory to create it.
        errno = (category && name_in_manifest(category)) ? ENOMEM : EINVAL;
        return -1;
    }
    return set_category_file(c, path ? path : "off");
}

bool log_get_category_stdout(const log_category_t *cat) {
    return cat && cat->to_stdout;
}

bool log_get_category_timestamp(const log_category_t *cat) {
    return cat && cat->timestamp;
}

bool log_get_category_show_pc(const log_category_t *cat) {
    return cat && cat->show_pc;
}

const char *log_get_category_file(const log_category_t *cat) {
    return cat ? cat->file_path : NULL;
}

// Public API ----------------------------------------------------------------

// Installs the context hooks (NULL removes them).
void log_set_context_hooks(const log_context_hooks_t *hooks) {
    s_hooks = hooks;
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

// Create every category the manifest declares, so `log.levels`
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
    struct log_category **slot = registry_slot(name);
    if (*slot)
        return *slot; // return existing; level unchanged (only manifest names get in)

    // A category that is not in the manifest is a typo, in code or in a
    // `log.set` argument.  It used to be created on the spot, which is how
    // `log.set cpuu 10` reported success and produced nothing.
    bool known = name_in_manifest(name);
    GS_ASSERTF(known, "log category '%s' is not in GS_LOG_CATEGORIES", name);
    if (!known)
        return NULL;

    struct log_category *c = create_category(name);
    if (!c)
        return NULL;

    // Index it, and insert at the enumeration list's head
    *slot = c;
    c->next = s_registry_head;
    s_registry_head = c;
    return c;
}

// Returns the category for 'name' or NULL if not found.
log_category_t *log_get_category(const char *name) {
    if (!name)
        return NULL;
    return registry_lookup(name);
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

// Visit every registered category, most recently registered first.
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

// A line being composed: it starts in the caller's stack buffer and moves to
// the heap only when a line outgrows it, so no line is clipped
typedef struct {
    char *p; // text, always NUL-terminated
    size_t len; // characters in p, excluding the NUL
    size_t cap; // bytes available at p
    bool heap; // p was malloc'd (and must be freed)
    bool truncated; // an allocation failed and text was dropped
} line_buf_t;

// Makes room for `need` more characters plus the NUL; false on OOM
static bool lb_reserve(line_buf_t *b, size_t need) {
    if (b->len + need + 1 <= b->cap)
        return true;
    size_t cap = b->cap * 2;
    while (cap < b->len + need + 1)
        cap *= 2;
    char *np = b->heap ? (char *)realloc(b->p, cap) : (char *)malloc(cap);
    if (!np)
        return false;
    // Leaving the stack buffer: carry over what was composed so far
    if (!b->heap)
        memcpy(np, b->p, b->len + 1);
    b->p = np;
    b->cap = cap;
    b->heap = true;
    return true;
}

// Appends n characters of s (clipped, and flagged, if memory runs out)
static void lb_append(line_buf_t *b, const char *s, size_t n) {
    if (!lb_reserve(b, n)) {
        n = b->cap - b->len - 1;
        b->truncated = true;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

// Appends printf-formatted text; false on a formatting error
static bool lb_vappendf(line_buf_t *b, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    size_t room = b->cap - b->len;
    int n = vsnprintf(b->p + b->len, room, fmt, ap);
    bool ok = n >= 0;
    if (!ok) {
        b->p[b->len] = '\0'; // drop whatever a failed vsnprintf left
    } else if ((size_t)n < room) {
        b->len += (size_t)n; // fitted first time (the common case)
    } else if (lb_reserve(b, (size_t)n)) {
        // Too long for the buffer: grow to the exact size and format again
        vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap2);
        b->len += (size_t)n;
    } else {
        b->len = b->cap - 1; // OOM: keep the clipped text vsnprintf wrote
        b->truncated = true;
    }
    va_end(ap2);
    return ok;
}

// Appends printf-formatted text (prefix pieces; cannot fail to format)
static void lb_appendf(line_buf_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    lb_vappendf(b, fmt, ap);
    va_end(ap);
}

// Sixty-four spaces, sliced for the indent (no per-line memset)
#define LOG_SPACES8 "        "
static const char k_indent_spaces[] =
    LOG_SPACES8 LOG_SPACES8 LOG_SPACES8 LOG_SPACES8 LOG_SPACES8 LOG_SPACES8 LOG_SPACES8 LOG_SPACES8;
#undef LOG_SPACES8
_Static_assert(sizeof(k_indent_spaces) == LOG_MAX_INDENT_SPACES + 1, "indent source must cover the clamp");

// Emits a log line using a va_list.
// Final form: "[name] level [@count] [PC=...] <indent>message\n"
void log_vemit(const log_category_t *cat, int level, const char *fmt, va_list ap) {
    // Same gate as the LOG macros, so a direct call honours the level too
    // (a missing category is treated as disabled)
    if (!log_would_log(cat, level))
        return;

    const struct log_category *c = (const struct log_category *)cat;

    // Compose prefix and body straight into one buffer (no second copy)
    char stack[768];
    line_buf_t b = {.p = stack, .len = 0, .cap = sizeof(stack), .heap = false, .truncated = false};
    stack[0] = '\0';

    lb_appendf(&b, "[%s] %d", c->name ? c->name : "", level);

    // Optional decorations, supplied by the context hooks: the instruction
    // count, then the PC
    if (c->timestamp) {
        unsigned long long t = (s_hooks && s_hooks->instr_count) ? s_hooks->instr_count() : 0;
        lb_appendf(&b, " @%llu", t);
    }
    if (c->show_pc) {
        char pcstr[48] = "PC=00000000";
        if (s_hooks && s_hooks->format_pc)
            s_hooks->format_pc(pcstr, sizeof(pcstr));
        lb_appendf(&b, " %s", pcstr);
    }
    lb_append(&b, " ", 1);

    // Indentation, then the message body
    if (s_indent_spaces > 0)
        lb_append(&b, k_indent_spaces, (size_t)s_indent_spaces);
    if (!lb_vappendf(&b, fmt ? fmt : "", ap)) {
        // Formatting error: emit nothing
        if (b.heap)
            free(b.p);
        return;
    }
    lb_append(&b, "\n", 1);

    // Out of memory part-way: mark the clipped line so it is not mistaken
    // for the whole message
    if (b.truncated && b.cap >= 5)
        memcpy(b.p + b.cap - 5, "...\n", 5);

    const char *line = b.p;

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

    // And to the context's observer (the debug trace capture)
    if (s_hooks && s_hooks->observe_line)
        s_hooks->observe_line(line);

    if (b.heap)
        free(b.p);
}

// Convenience wrapper for variadic emission.
void log_emit(const log_category_t *cat, int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vemit(cat, level, fmt, ap);
    va_end(ap);
}
