// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// log.h
// Public logging API: per-category runtime levels with zero-cost disabled paths.

#ifndef LOG_H
#define LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque category handle
typedef struct log_category log_category_t;

// Initialization (idempotent). Registers the `log` shell command.
void log_init(void);

// Category management -------------------------------------------------------
// Registers a category (or returns existing). On first creation, level = 0.
// Returns NULL on OOM or invalid name.
log_category_t *log_register_category(const char *name);

// Lookup by name (case-sensitive). Returns NULL when not found.
log_category_t *log_get_category(const char *name);

// Introspection
const char *log_category_name(const log_category_t *cat);
int log_get_level(const log_category_t *cat);
int log_set_level(log_category_t *cat, int level); // returns previous level or negative on error

// Output sink ---------------------------------------------------------------
// Sink invoked for each fully formatted line (including trailing '\n').
typedef void (*log_sink_fn)(const char *line, void *user);
void log_set_sink(log_sink_fn fn, void *user); // defaults to stdout sink

// Indentation control ------------------------------------------------------
// Adjusts the leading spaces inserted before each message body.
void log_indent_set(int spaces);
int log_indent_get(void);
void log_indent_adjust(int delta);
#define LOG_INDENT(delta) log_indent_adjust((delta))

// Emission ------------------------------------------------------------------
// printf-style emit functions. Prefer using the LOG/LOG_WITH macros below.
void log_emit(const log_category_t *cat, int level, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
    ;

void log_vemit(const log_category_t *cat, int level, const char *fmt, va_list ap);

// Fast-path predicate -------------------------------------------------------
// Returns non-zero if a message at 'level' for 'cat' would be emitted.
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
// '_log_get_local_category'.
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
    static inline log_category_t *_log_get_local_category(void) {                                                      \
        return (catptr);                                                                                               \
    }

// Define a file-local accessor with lazy registration by name.
#define LOG_USE_CATEGORY_NAME(name)                                                                                    \
    static log_category_t *_log_local_category_ptr = 0;                                                                \
    static inline log_category_t *_log_get_local_category(void) {                                                      \
        if (!_log_local_category_ptr)                                                                                  \
            _log_local_category_ptr = log_register_category((name));                                                   \
        return _log_local_category_ptr;                                                                                \
    }

// Optional helper to declare a pointer intended for LOG_USE_CATEGORY.
#define LOG_DECLARE_LOCAL_CATEGORY(var) static log_category_t *var = 0

// LOG macros ----------------------------------------------------------------
// Preferred: LOG(level, fmt, ...) using the file's implicit category
// Explicit:  LOG_WITH(cat, level, fmt, ...)

#ifndef LOG_COMPILE_MIN_LEVEL
#define LOG_COMPILE_MIN_LEVEL 0
#endif

#if defined(__GNUC__)
#define LOG_WITH(cat, level, fmt, ...)                                                                                 \
    do {                                                                                                               \
        const log_category_t *_lg_cat = (cat);                                                                         \
        const int _lg_lvl = (level);                                                                                   \
        if (__builtin_expect(log_would_log(_lg_cat, _lg_lvl), 0))                                                      \
            log_emit(_lg_cat, _lg_lvl, (fmt), ##__VA_ARGS__);                                                          \
    } while (0)
#else
#define LOG_WITH(cat, level, fmt, ...)                                                                                 \
    do {                                                                                                               \
        const log_category_t *_lg_cat = (cat);                                                                         \
        const int _lg_lvl = (level);                                                                                   \
        if (log_would_log(_lg_cat, _lg_lvl))                                                                           \
            log_emit(_lg_cat, _lg_lvl, (fmt), ##__VA_ARGS__);                                                          \
    } while (0)
#endif

#define LOG(level, fmt, ...) LOG_WITH(_log_get_local_category(), (level), (fmt), ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // LOG_H
