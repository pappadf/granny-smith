#ifndef LOG_H
#define LOG_H
/* Minimal no-op logging API for unit tests.
   Included via -include to avoid depending on full logging runtime. */
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Opaque category handle placeholder */
typedef struct log_category { int _dummy; } log_category_t;

/* Sink type placeholder */
typedef void (*log_sink_fn)(const char* line, void* user);

/* No-op functions to satisfy references */
static inline void log_init(void) {}
static inline log_category_t* log_register_category(const char* name) { (void)name; static log_category_t c; return &c; }
static inline log_category_t* log_get_category(const char* name) { (void)name; static log_category_t c; return &c; }
static inline const char* log_category_name(const log_category_t* cat) { (void)cat; return ""; }
static inline int  log_get_level(const log_category_t* cat) { (void)cat; return 0; }
static inline int  log_set_level(log_category_t* cat, int level) { (void)cat; (void)level; return 0; }
static inline void log_set_sink(log_sink_fn fn, void* user) { (void)fn; (void)user; }
static inline void log_emit(const log_category_t* cat, int level, const char* fmt, ...) { (void)cat; (void)level; (void)fmt; }
static inline void log_vemit(const log_category_t* cat, int level, const char* fmt, va_list ap) { (void)cat; (void)level; (void)fmt; (void)ap; }

/* Macros and predicates (no-op) */
static inline int log_would_log(const log_category_t* cat, int level) { (void)cat; (void)level; return 0; }

#define LOG_USE_CATEGORY(catptr) \
    static inline log_category_t* _log_get_local_category(void) { return (catptr); }

#define LOG_USE_CATEGORY_NAME(name) \
    static log_category_t _log_dummy_cat; \
    static inline log_category_t* _log_get_local_category(void) { (void)(name); return &_log_dummy_cat; }

#ifndef LOG_COMPILE_MIN_LEVEL
#define LOG_COMPILE_MIN_LEVEL 0
#endif

#define LOG_WITH(cat, level, fmt, ...) do { (void)(cat); (void)(level); (void)(fmt); } while (0)
#define LOG(level, fmt, ...)          do { (void)(level); (void)(fmt); } while (0)

#ifdef __cplusplus
}
#endif
#endif /* LOG_H */
