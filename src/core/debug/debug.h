// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug.h
// Public interface for debugging, breakpoints, and tracing.

#ifndef DEBUG_H
#define DEBUG_H

// === Includes ===
#include "addr_format.h"
#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Forward Declarations ===
struct cpu;
struct object;

struct breakpoint;
typedef struct breakpoint breakpoint_t;

struct logpoint;
typedef struct logpoint logpoint_t;

// Logpoint kind.  PC logpoints fire when the CPU executes at an address range
// (cheap; checked in the debug step hook).  Memory logpoints fire on reads or
// writes to a physical/logical address range (forces the page through the
// memory slow path).
enum logpoint_kind {
    LP_KIND_PC = 0, // fire on PC match
    LP_KIND_WRITE = 1, // fire on memory write
    LP_KIND_READ = 2, // fire on memory read
    LP_KIND_RW = 3, // fire on read or write
};

struct config;
typedef struct config config_t;

// === Constants ===
#define TRACE_ENTRY_PC  0
#define TRACE_ENTRY_LOG 1

// === Type Definitions ===

// Single trace entry: either a PC value or a log message index
typedef struct trace_entry {
    uint8_t type; // TRACE_ENTRY_PC or TRACE_ENTRY_LOG
    uint32_t value; // PC address or log message index
} trace_entry_t;

// Log message stored in trace log buffer
typedef struct trace_log_msg {
    char *text; // Log message text (owned)
} trace_log_msg_t;

// Debug state (exposed for performance-critical access)
struct debug {
    bool active;
    int step;
    breakpoint_t *breakpoints;
    uint32_t last_breakpoint_pc; // Track last breakpoint PC hit to skip it once when resuming
    logpoint_t *logpoints;
    // Sparse stable id counters (proposal §2.1). Incremented on every
    // add; never reset, never recycled. The first allocated id is 0.
    int next_breakpoint_id;
    int next_logpoint_id;
    // Trace buffer for PC entries
    uint32_t *trace_buffer;
    uint32_t trace_buffer_size;
    int trace_head;
    int trace_tail;
    int trace_size;
    // Trace log message buffer
    trace_log_msg_t *trace_log_buffer;
    uint32_t trace_log_buffer_size;
    uint32_t trace_log_head;
    uint32_t trace_log_count;
    // Combined trace entries (PC + log references)
    trace_entry_t *trace_entries;
    uint32_t trace_entries_size;
    uint32_t trace_entries_head;
    uint32_t trace_entries_tail;
    // Platform-specific assertion callback (e.g., for test integration)
    void (*assertion_callback)(const char *expr, const char *file, int line, const char *func);
    // Object-tree binding — lifetime tied to debug_init / debug_cleanup.
    struct object *debugger_object;
    struct object *bp_collection_object;
    struct object *lp_collection_object;
};

struct debug;
typedef struct debug debug_t;

// === Lifecycle (Constructor / Destructor) ===

debug_t *debug_init(void);
void debug_cleanup(debug_t *debug);

// === Operations ===

breakpoint_t *set_breakpoint(debug_t *debug, uint32_t addr, addr_space_t space);

bool delete_breakpoint(debug_t *debug, uint32_t addr, addr_space_t space);

// Bulk break/logpoint management — used by typed root wrappers as direct
// implementations (no shell_dispatch indirection). list_* prints to
// stdout; delete_all_* returns the count of entries removed.
void list_breakpoints(debug_t *debug);
int delete_all_breakpoints(debug_t *debug);
void list_logpoints(debug_t *debug);
int delete_all_logpoints(debug_t *debug);

// argv-driven entry points for the legacy `print` / `examine` /
// `logpoint` / `find` commands. The typed object-model bridge tokenises
// its spec strings via shell_tokenize and calls these directly so the
// rich-parser command bodies stay in one place.
int64_t shell_print_argv(int argc, char **argv);
int shell_examine_argv(int argc, char **argv);
int shell_logpoint_argv(int argc, char **argv);
int shell_find_argv(int argc, char **argv);

// Truthiness check used by typed `assert` root method. Strings like
// "false", "0", or formatted-error tails are falsy; everything else
// (including the empty/whitespace-only result of an unknown enum) is
// truthy.  The rule mirrors proposal §2.5.
bool predicate_is_truthy(const char *s);

// Register / FPU / Mac-state dumps — used by typed `info_*` wrappers
// and the legacy `td` / `fpregs` / `mac-state` commands. Both layers
// share the same printer; the typed wrapper has no cmd_context so
// stdout is the only output target.
void debug_print_regs(void);
void debug_print_fpregs(void);
void debug_print_mac_state(void);

// Framebuffer utilities — used by typed `screen.*` wrappers and the
// legacy `screenshot` command. Both layers call into the same
// primitives so neither has to know about the other.
#define DEBUG_SCREEN_WIDTH  512
#define DEBUG_SCREEN_HEIGHT 342
uint32_t framebuffer_checksum(const uint8_t *fb);
uint32_t framebuffer_region_checksum(const uint8_t *fb, int top, int left, int bottom, int right);
int match_framebuffer_with_png(const uint8_t *fb, const char *filename);
int save_framebuffer_as_png(const uint8_t *fb, const char *filename);

// === M6: object-model accessors ============================================
//
// debugger.{breakpoints,logpoints}.add(...) / .N.remove() and the
// per-entry attribute getters live in src/core/object/debug_classes.c.
// They reach into the debug_t lists via these accessors so the
// debug.c internals stay private (struct breakpoint / struct logpoint
// definitions live in debug.c).
//
// Identity: every breakpoint and logpoint carries a sparse stable id
// (proposal §2.1 — never recycled, max-id-ever + 1 on add). The id is
// what the indexed-child callbacks expose as the "index" segment.

// Allocate a fresh sparse id. Caller assigns it to its entry struct
// before linking it into the list. Pure helper, no side effects beyond
// bumping the counter.
int debug_alloc_breakpoint_id(debug_t *debug);
int debug_alloc_logpoint_id(debug_t *debug);

// Look up an entry by sparse id. Returns NULL if no live entry has
// this id. Used by indexed-child get(parent, index).
breakpoint_t *debug_breakpoint_by_id(debug_t *debug, int id);
logpoint_t *debug_logpoint_by_id(debug_t *debug, int id);

// Walk the live entries in id order. count() returns the live entry
// count; next(prev) returns the smallest id strictly greater than prev,
// or -1 if no more entries (prev = -1 returns the smallest live id).
int debug_breakpoint_count(debug_t *debug);
int debug_breakpoint_next_id(debug_t *debug, int prev_id);
int debug_logpoint_count(debug_t *debug);
int debug_logpoint_next_id(debug_t *debug, int prev_id);

// Remove by sparse id. Returns true if an entry was removed. Frees the
// entry's attached object_t (which fires invalidators) before freeing
// the entry struct.
bool debug_remove_breakpoint(debug_t *debug, int id);
bool debug_remove_logpoint(debug_t *debug, int id);

// Per-entry attribute getters (read-only at this stage; setters arrive
// alongside writable conditions / messages in a future milestone).
uint32_t breakpoint_get_addr(const breakpoint_t *bp);
int breakpoint_get_space(const breakpoint_t *bp); // 0 = LOGICAL, 1 = PHYSICAL
const char *breakpoint_get_condition(const breakpoint_t *bp); // NULL if none
uint32_t breakpoint_get_hit_count(const breakpoint_t *bp);
int breakpoint_get_id(const breakpoint_t *bp);
struct object *breakpoint_get_entry_object(const breakpoint_t *bp);
// Replace the condition string. NULL clears it. Takes ownership semantics
// via strdup; caller's buffer is not retained.
void breakpoint_set_condition(breakpoint_t *bp, const char *expr);

uint32_t logpoint_get_addr(const logpoint_t *lp);
uint32_t logpoint_get_end_addr(const logpoint_t *lp);
int logpoint_get_kind(const logpoint_t *lp); // enum logpoint_kind
int logpoint_get_level(const logpoint_t *lp);
const char *logpoint_get_category_name(const logpoint_t *lp);
const char *logpoint_get_message(const logpoint_t *lp);
uint32_t logpoint_get_hit_count(const logpoint_t *lp);
int logpoint_get_id(const logpoint_t *lp);
struct object *logpoint_get_entry_object(const logpoint_t *lp);

void debugger_disasm_pc(char *buf, size_t buf_size);

int debugger_disasm(char *buf, size_t buf_size, uint32_t addr);

int debug_break_and_trace(void);

void debug_print_target_trace(void);

void debug_trace_capture_log(const char *line);

int debug_trace_is_active(void);

bool debug_active(debug_t *debug);

// Check if prompt/status line is enabled (IMP-308)
int debug_prompt_enabled(void);

// Set the default for prompt (used by --no-prompt CLI flag so that every
// subsequent shell connection inherits the setting without having to send
// "prompt off" first).
void debug_set_prompt_default(int enabled);

// === Exception trace ring ===
// Records every CPU bus error / exception as a ring buffer entry.  The
// bus-error code paths in cpu_internal.h call exc_trace_record().  Enable
// streaming with `log exceptions 1`, dump the ring with `info exceptions`.
typedef struct exc_trace_entry {
    uint64_t ts; // CPU instruction count at the exception
    uint32_t faulting_pc; // cpu->instruction_pc when the fault occurred
    uint32_t saved_pc; // PC stacked (differs from faulting_pc on retry vs skip)
    uint32_t fault_addr; // faulting address written to stack frame
    uint32_t vbr;
    uint32_t vector; // exception vector (0x008 = bus error, etc.)
    uint16_t sr;
    uint16_t format_frame; // 0xA/0xB/0x0/0x2
    uint8_t rw; // 1=read, 0=write
    uint8_t double_fault_kind; // 0 = none, 1 = retry double-fault detected
} exc_trace_entry_t;

// Record one exception event (called from cpu_internal.h exception paths)
void exc_trace_record(uint32_t vector, uint32_t faulting_pc, uint32_t saved_pc, uint32_t fault_addr, uint32_t rw,
                      uint32_t vbr, uint16_t sr, uint16_t format_frame, int double_fault_kind);

#endif // DEBUG_H
