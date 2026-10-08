// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// log.h
// Public logging API: per-category runtime levels with zero-cost disabled paths.

#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque category handle
typedef struct log_category log_category_t;

// Category management -------------------------------------------------------
// Registers a category (or returns existing). On first creation, level = 0.
// Returns NULL on OOM or invalid name.
log_category_t *log_register_category(const char *name);

// Create every category GS_LOG_CATEGORIES declares.  Called once from
// setup_init so `log.levels` and `log.category` list the complete set rather
// than only what has been hit so far.
void log_register_manifest(void);

// === Typed per-category configuration ===
//
// These replace log_configure(category, "level=5 stdout=off ..."), a flag
// grammar inside a string that the framework could not validate and
// completion could not offer.  Each takes the category by name and validates
// it against the manifest, so a typo is rejected here rather than silently
// creating a category that can never emit.
//
// All return 0 on success, -1 on an unknown category or a bad value
// (log_set_category_file: also when the file cannot be opened, with errno
// left as fopen set it).
int log_set_category_level(const char *category, int level);
int log_set_category_stdout(const char *category, bool on);
int log_set_category_timestamp(const char *category, bool on);
int log_set_category_show_pc(const char *category, bool on);
int log_set_category_file(const char *category, const char *path); // NULL/"off" closes

// Per-category configuration getters (the log.category[...] attributes).
bool log_get_category_stdout(const log_category_t *cat);
bool log_get_category_timestamp(const log_category_t *cat);
bool log_get_category_show_pc(const log_category_t *cat);
const char *log_get_category_file(const log_category_t *cat); // NULL when no file sink

// One-line description from the manifest, or NULL for an unknown name.
const char *log_category_description(const char *name);

// Lookup by name (case-sensitive). Returns NULL when not found.
log_category_t *log_get_category(const char *name);

// Introspection
const char *log_category_name(const log_category_t *cat);
int log_get_level(const log_category_t *cat);
int log_set_level(log_category_t *cat, int level); // returns previous level or negative on error

// Visit every registered category, most recently registered first (for UI enumeration).
void log_foreach_category(void (*fn)(const log_category_t *cat, void *ud), void *ud);

// Output sink ---------------------------------------------------------------
// Sink invoked for each fully formatted line (including trailing '\n').
typedef void (*log_sink_fn)(const char *line, void *user);
void log_set_sink(log_sink_fn fn, void *user); // NULL: only the per-category stdout/file sinks

// Context hooks ---------------------------------------------------------------
// The logger is a leaf: what it knows about the running machine comes in
// through these, installed by log_context.c.  Any member may be NULL.
typedef struct log_context_hooks {
    unsigned long long (*instr_count)(void); // the `@count` timestamp decoration
    void (*format_pc)(char *buf, size_t size); // the PC decoration ("PC=...")
    void (*observe_line)(const char *line); // sees every emitted line (debug trace)
} log_context_hooks_t;
void log_set_context_hooks(const log_context_hooks_t *hooks); // NULL removes them

// Indentation control ------------------------------------------------------
// Adjusts the leading spaces inserted before each message body.
void log_indent_set(int spaces);
int log_indent_get(void);
void log_indent_adjust(int delta);
#define LOG_INDENT(delta) log_indent_adjust((delta))

// Emission ------------------------------------------------------------------
// printf-style emit functions. Prefer using the LOG/LOG_WITH macros below.
// Both apply the same level gate as the macros (log_would_log).  A line has
// no length limit: it is composed on the stack and moves to the heap when it
// outgrows it.
void log_emit(const log_category_t *cat, int level, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
    ;

void log_vemit(const log_category_t *cat, int level, const char *fmt, va_list ap);

// Fast-path predicate -------------------------------------------------------
// Returns non-zero if a message at 'level' for 'cat' would be emitted.
// The comparison is `<=` on purpose: level 0 is the always-on level, so a
// LOG(0, ...) site (errors, one-off reports) emits for every registered
// category, while a category at 0 ("off") silences its level-1-and-up sites
// (docs/internals/core/debug/log.md, "Level 0 is the always-on level").
static inline int log_would_log(const log_category_t *cat, int level) {
#if defined(LOG_COMPILE_MIN_LEVEL)
#if defined(__GNUC__)
    if (__builtin_constant_p(level) && (level < LOG_COMPILE_MIN_LEVEL))
        return 0;
#else
    if (level < LOG_COMPILE_MIN_LEVEL)
        return 0;
#endif
#endif
    // If no category, treat as disabled.
    return cat != 0 && level <= log_get_level(cat);
}

// ----------------------------------------------------------------------------
// Implicit per-file category support
//
// Files can set an implicit category once, enabling calls like:
//     LOG(40, "value=%d", v);
// If not set, using LOG(...) should trigger a compile error due to missing
// 'log_local_category'.
//
// Patterns:
//   1) One-liner (lazy init):
//        LOG_USE_CATEGORY_NAME("cpu");
//   2) Two-step:
//        static log_category_t* cpu_cat;
//        LOG_USE_CATEGORY(cpu_cat);
//        ... cpu_cat = log_register_category("cpu");
// ----------------------------------------------------------------------------

// Define a file-local accessor returning the implicit category pointer.
#define LOG_USE_CATEGORY(catptr)                                                                                       \
    static inline log_category_t *log_local_category(void) {                                                           \
        return (catptr);                                                                                               \
    }

// Define a file-local accessor with lazy registration by name.
#define LOG_USE_CATEGORY_NAME(name)                                                                                    \
    static log_category_t *log_local_category_ptr = 0;                                                                 \
    static inline log_category_t *log_local_category(void) {                                                           \
        if (!log_local_category_ptr)                                                                                   \
            log_local_category_ptr = log_register_category((name));                                                    \
        return log_local_category_ptr;                                                                                 \
    }

// Optional helper to declare a pointer intended for LOG_USE_CATEGORY.
#define LOG_DECLARE_LOCAL_CATEGORY(var) static log_category_t *var = 0

// LOG macros ----------------------------------------------------------------
// Preferred: LOG(level, fmt, ...) using the file's implicit category
// Explicit:  LOG_WITH(cat, level, fmt, ...)
// `, ##__VA_ARGS__` is the GNU comma swallow; the project is GCC/Clang-only
// (docs/guide/STYLE_GUIDE.md, "Compiler Extensions").

#ifndef LOG_COMPILE_MIN_LEVEL
#define LOG_COMPILE_MIN_LEVEL 0
#endif

#define LOG_WITH(cat, level, fmt, ...)                                                                                 \
    do {                                                                                                               \
        const log_category_t *_lg_cat = (cat);                                                                         \
        const int _lg_lvl = (level);                                                                                   \
        if (__builtin_expect(log_would_log(_lg_cat, _lg_lvl), 0))                                                      \
            log_emit(_lg_cat, _lg_lvl, (fmt), ##__VA_ARGS__);                                                          \
    } while (0)

#define LOG(level, fmt, ...) LOG_WITH(log_local_category(), (level), (fmt), ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // LOG_H
