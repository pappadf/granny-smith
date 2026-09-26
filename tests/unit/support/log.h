// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
#ifndef LOG_H
#define LOG_H
/* Minimal no-op logging API for unit tests.
   Included via -include to avoid depending on full logging runtime. */
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Opaque category handle placeholder */
typedef struct log_category {
    int _dummy;
} log_category_t;

/* Sink type placeholder */
typedef void (*log_sink_fn)(const char *line, void *user);

/* No-op functions to satisfy references */
static inline void log_init(void) {}
static inline log_category_t *log_register_category(const char *name) {
    (void)name;
    static log_category_t c;
    return &c;
}
static inline log_category_t *log_get_category(const char *name) {
    (void)name;
    static log_category_t c;
    return &c;
}
static inline const char *log_category_name(const log_category_t *cat) {
    (void)cat;
    return "";
}
static inline int log_get_level(const log_category_t *cat) {
    (void)cat;
    return 0;
}
static inline int log_set_level(log_category_t *cat, int level) {
    (void)cat;
    (void)level;
    return 0;
}
static inline void log_set_sink(log_sink_fn fn, void *user) {
    (void)fn;
    (void)user;
}
static inline void log_emit(const log_category_t *cat, int level, const char *fmt, ...) {
    (void)cat;
    (void)level;
    (void)fmt;
}
static inline void log_vemit(const log_category_t *cat, int level, const char *fmt, va_list ap) {
    (void)cat;
    (void)level;
    (void)fmt;
    (void)ap;
}

/* Indentation control, as in the real log.h */
static inline void log_indent_set(int spaces) {
    (void)spaces;
}
static inline int log_indent_get(void) {
    return 0;
}
static inline void log_indent_adjust(int delta) {
    (void)delta;
}
#define LOG_INDENT(delta) log_indent_adjust((delta))

/* Macros and predicates (no-op) */
static inline int log_would_log(const log_category_t *cat, int level) {
    (void)cat;
    (void)level;
    return 0;
}

#define LOG_USE_CATEGORY(catptr)                                                                                       \
    static inline log_category_t *_log_get_local_category(void) {                                                      \
        return (catptr);                                                                                               \
    }

#define LOG_USE_CATEGORY_NAME(name)                                                                                    \
    static log_category_t _log_dummy_cat;                                                                              \
    static inline log_category_t *_log_get_local_category(void) {                                                      \
        (void)(name);                                                                                                  \
        return &_log_dummy_cat;                                                                                        \
    }

#ifndef LOG_COMPILE_MIN_LEVEL
#define LOG_COMPILE_MIN_LEVEL 0
#endif

/* Never-called sinks for LOG / LOG_WITH: taking the arguments type-checks them
   (format attribute included) and counts them as used, as the real macros do */
static inline void log_noop_(int level, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;
static inline void log_noop_(int level, const char *fmt, ...) {
    (void)level;
    (void)fmt;
}
static inline void log_noop_with_(const log_category_t *cat, int level, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
    ;
static inline void log_noop_with_(const log_category_t *cat, int level, const char *fmt, ...) {
    (void)cat;
    (void)level;
    (void)fmt;
}

/* The arguments are compiled but never evaluated (if (0)) */
#define LOG_WITH(cat, level, fmt, ...)                                                                                 \
    do {                                                                                                               \
        if (0)                                                                                                         \
            log_noop_with_((cat), (level), fmt, ##__VA_ARGS__);                                                        \
    } while (0)
#define LOG(level, fmt, ...)                                                                                           \
    do {                                                                                                               \
        if (0)                                                                                                         \
            log_noop_((level), fmt, ##__VA_ARGS__);                                                                    \
    } while (0)

#ifdef __cplusplus
}
#endif
#endif /* LOG_H */
